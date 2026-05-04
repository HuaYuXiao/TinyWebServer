#ifndef REDIS_CACHE_H
#define REDIS_CACHE_H

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bloom_filter.h"
#include "circuit_breaker.h"
#include "redis_pool.h"

// Redis 缓存工具类 —— 考研成绩查询系统
//
// 核心能力:
//   - 防缓存穿透: 布隆过滤器 + 空值缓存（短 TTL）
//   - 防缓存击穿: 分布式互斥锁 (SETNX)，仅一个线程重建缓存
//   - 防缓存雪崩: 随机 TTL 抖动 (±10%)，避免集中过期
//   - 容错降级: 熔断器 + 直连数据库回退
//
// 单例模式，线程安全。
class RedisCache {
public:
  static RedisCache *GetInstance();

  // 初始化（须在 Redis 连接池 init 之后调用）
  void init(redis_pool *pool);

  // ── 核心缓存操作 ────────────────────────────────────

  // 带三级防护的缓存读取
  //   key:      缓存键
  //   db_query: 缓存未命中时的 DB 查询回调（返回值 nullopt 表示记录不存在）
  //   base_ttl: 基础过期时间（秒），默认 3600，实际写入会加随机抖动
  std::optional<std::string> get(const std::string &key,
                                 std::function<std::optional<std::string>()> db_query,
                                 int base_ttl = 3600);

  // 写入缓存（含随机 TTL 防雪崩）
  bool set(const std::string &key, const std::string &value, int base_ttl = 3600);

  // 删除缓存（Cache Aside 模式：先写 DB 再删缓存）
  bool del(const std::string &key);

  // 批量读取
  std::vector<std::optional<std::string>>
  mget(const std::vector<std::string> &keys,
       std::function<std::optional<std::string>(const std::string &)> db_query,
       int base_ttl = 3600);

  // 预热布隆过滤器（从 DB 加载已有键集合，防止冷启动穿透）
  void warm_bloom(const std::vector<std::string> &keys);

  // 重置熔断器（运维接口）
  void reset_breaker() { circuit_breaker_.reset(); }

  CircuitState breaker_state() const { return circuit_breaker_.state(); }

  // 是否已预热
  bool is_bloom_warmed() const { return bloom_warmed_; }

private:
  RedisCache() = default;

  // 底层 Redis 操作
  std::optional<std::string> redis_raw_get(redisContext *ctx,
                                           const std::string &key);
  bool redis_raw_set(redisContext *ctx, const std::string &key,
                     const std::string &value, int ttl);
  bool redis_raw_del(redisContext *ctx, const std::string &key);

  // 分布式锁 — SETNX + TTL，防击穿
  bool try_lock(redisContext *ctx, const std::string &key, int lock_ttl = 10);
  void unlock(redisContext *ctx, const std::string &key);

  // 随机 TTL: base ± 10%，防雪崩
  int random_ttl(int base_ttl) const;

  redis_pool *pool_ = nullptr;
  BloomFilter bloom_filter_;
  CircuitBreaker circuit_breaker_;
  bool bloom_warmed_ = false;
  std::mutex bloom_warm_mutex_;

  static constexpr int NULL_CACHE_TTL = 60;   // 空值缓存 TTL（秒）
  static constexpr int LOCK_TTL = 10;         // 互斥锁 TTL（秒）
  static constexpr int RETRY_SLEEP_MS = 100;  // 未获锁时重试间隔
  static constexpr int MAX_RETRIES = 5;       // 最大重试次数
};

#endif
