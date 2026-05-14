#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include "token_bucket.h"

#include <arpa/inet.h>
#include <mutex>
#include <string>
#include <unordered_map>

// ── IP 级别令牌桶限流器（单例）────────────────────────────────────
//
// 按 (IP, endpoint_type) 二元组进行独立限流，不同接口互不影响。
// 令牌桶在内存中以 unordered_map 管理，配合定时清理防止 IP 条目无限膨胀。
//
// 使用方式:
//   1. 从 sockaddr_in 提取 IP 字符串
//   2. RateLimiter::GetInstance()->allow(ip, endpoint_type) → true/false
//   3. false → 返回 HTTP 429 Too Many Requests
//
// endpoint_type 分类:
//   "register" - /auth/register  (防注册洪水)
//   "login"    - /auth/login     (防暴力破解)
//   "api"      - /api/*          (防 CRUD 滥用)
//   "global"   - 兜底            (/4 查询、静态文件)
//
// 并发安全: 所有公开方法内部持有 std::mutex，多 worker 线程安全访问。

class RateLimiter {
public:
  static RateLimiter *GetInstance() {
    static RateLimiter instance;
    return &instance;
  }

  // ── IP 转换工具 ──────────────────────────────────────────────────
  // 将 sockaddr_in 转为 "x.x.x.x" 字符串
  static std::string ip_to_str(const sockaddr_in &addr) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return buf;
  }

  // ── 核心: 请求准入判断 ──────────────────────────────────────────
  // ip: 客户端 IP 字符串
  // endpoint: 接口分类 ("register" / "login" / "api" / "global")
  // cost: 普通请求=1，批量操作可调高
  bool allow(const std::string &ip, const std::string &endpoint,
             int cost = 1) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &bucket = get_or_create(ip, endpoint);
    return bucket.consume(cost);
  }

  // ── 连接级限流: accept 后检查 IP 是否允许建立新连接 ─────────────
  // 比请求级更粗粒度，用于在 accept 阶段就拦截连接洪水
  bool allow_connection(const std::string &ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &bucket = get_or_create(ip, "connect");
    return bucket.consume();
  }

  // ── 定时清理: 删除超过 N 秒无活动的 IP 条目 ─────────────────────
  // 建议在 timer_handler() 中调用，避免 map 无限膨胀
  void cleanup_idle(int idle_seconds = 120) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.begin();
    while (it != buckets_.end()) {
      if (it->second->seconds_since_refill() > idle_seconds) {
        it = buckets_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // ── 查询接口（调试 / 监控用）────────────────────────────────────
  size_t bucket_count() const {
    // 注: 这里不加锁仅用于粗略监控，生产环境可接受
    return buckets_.size();
  }

private:
  RateLimiter() = default;

  // 根据 ip + endpoint 组成唯一 key，查找或创建 TokenBucket
  // 调用者需持有 mutex_
  TokenBucket &get_or_create(const std::string &ip,
                             const std::string &endpoint) {
    std::string key = endpoint + ":" + ip;
    auto it = buckets_.find(key);
    if (it != buckets_.end()) {
      return *it->second;
    }

    // 根据 endpoint 类型选择限流参数
    double rate, burst;
    if (endpoint == "register") {
      rate = 2.0;  // 每秒 2 次注册
      burst = 5.0; // 突发最多 5 次
    } else if (endpoint == "login") {
      rate = 5.0;  // 每秒 5 次登录
      burst = 10.0; // 突发最多 10 次
    } else if (endpoint == "api") {
      rate = 10.0;  // 每秒 10 次 CRUD 操作
      burst = 20.0;
    } else if (endpoint == "connect") {
      rate = 100.0; // 每秒允许 100 个新连接
      burst = 200.0; // 突发最多 200
    } else {
      // global: /4 查询、静态文件
      rate = 50.0;   // 每秒 50 次
      burst = 100.0; // 突发最多 100 次
    }

    auto bucket = std::make_unique<TokenBucket>(rate, burst);
    TokenBucket &ref = *bucket;
    buckets_[key] = std::move(bucket);
    return ref;
  }

  std::mutex mutex_;
  std::unordered_map<std::string, std::unique_ptr<TokenBucket>> buckets_;
};

#endif
