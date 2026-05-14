#ifndef RATE_LIMITER_TOKEN_BUCKET_H
#define RATE_LIMITER_TOKEN_BUCKET_H

#include <algorithm>
#include <chrono>
#include <mutex>

// 令牌桶算法：支持平滑限流 + 突发（burst）
//
// refill:  每秒补充 rate 个令牌，上限 burst
// consume: 每次请求消耗 cost 个令牌，令牌不足则拒绝
//
// 为什么令牌桶适合本项目的 DDoS 防御？
//   漏桶 - 严格匀速，不允许突发 → 压测场景下合法突发会被误杀
//   固定窗口 - 窗口边界会有"翻倍"问题（窗口 1 末尾和窗口 2 开头同时有请求）
//   滑动窗口 - 更精确但实现依赖 Redis sorted set，网络开销大
//   令牌桶 - 允许短时间内积攒 burst 个令牌用于突发, 同时限制长时间平均速率
//           纯本地内存操作，零网络开销，适合 C++ 单进程场景

class TokenBucket {
public:
  // rate: 每秒补充的令牌数
  // burst: 令牌上限（允许的最大突发量）
  // 最后两个参数供默认构造用
  TokenBucket(double rate = 20.0, double burst = 40.0)
      : rate_(rate), burst_(burst), tokens_(burst),
        last_refill_(std::chrono::steady_clock::now()) {}

  // 尝试消耗 cost 个令牌，成功返回 true
  bool consume(int cost = 1) {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    if (tokens_ >= cost) {
      tokens_ -= cost;
      return true;
    }
    return false;
  }

  // 查看当前可用令牌数（不消耗，调试用）
  double available() {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    return tokens_;
  }

  // 上次 refill 后过了多少秒（用于判断是否长时间无活动，超过阈值可被清理）
  double seconds_since_refill() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - last_refill_).count();
  }

private:
  // 根据自上次补充以来流逝的时间，按 rate_ 速率补充令牌
  // 调用者需持有 mutex_
  void refill() {
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - last_refill_).count();
    tokens_ = std::min(burst_, tokens_ + rate_ * elapsed);
    last_refill_ = now;
  }

  double rate_;
  double burst_;
  double tokens_;
  std::chrono::steady_clock::time_point last_refill_;
  std::mutex mutex_;
};

#endif
