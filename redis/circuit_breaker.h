#ifndef CIRCUIT_BREAKER_H
#define CIRCUIT_BREAKER_H

#include <atomic>
#include <chrono>
#include <mutex>

// 熔断器 —— 缓存服务不可用时自动降级，防止服务雪崩
//
// 三态状态机:
//   CLOSED    → 正常请求，连续失败达阈值则 → OPEN
//   OPEN      → 快速失败（不访问 Redis），超时后 → HALF_OPEN
//   HALF_OPEN → 放行少量探测请求，成功则 → CLOSED，失败则 → OPEN

enum class CircuitState {
  CLOSED,     // 正常
  OPEN,       // 熔断
  HALF_OPEN   // 半开（探测恢复）
};

class CircuitBreaker {
public:
  // fail_threshold: 连续失败多少次后熔断
  // recovery_timeout_ms: 熔断多久后进入半开尝试恢复
  // half_open_max: 半开状态最多放行多少个探测请求
  CircuitBreaker(int fail_threshold = 5,
                 int recovery_timeout_ms = 30000,
                 int half_open_max = 3)
      : fail_threshold_(fail_threshold),
        recovery_timeout_ms_(recovery_timeout_ms),
        half_open_max_(half_open_max) {}

  // 检查是否允许通过，返回 true 表示可以尝试调用
  bool allow_request() {
    switch (state_.load()) {
    case CircuitState::CLOSED:
      return true;

    case CircuitState::OPEN: {
      auto elapsed = std::chrono::steady_clock::now() - opened_at_;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
          >= recovery_timeout_ms_) {
        state_.store(CircuitState::HALF_OPEN);
        half_open_count_.store(0);
        return true;
      }
      return false;
    }

    case CircuitState::HALF_OPEN:
      return half_open_count_.fetch_add(1) < half_open_max_;
    }
    return false;
  }

  // 调用成功时上报
  void on_success() {
    fail_count_.store(0);
    if (state_.load() == CircuitState::HALF_OPEN) {
      state_.store(CircuitState::CLOSED);
    }
  }

  // 调用失败时上报
  void on_failure() {
    int count = fail_count_.fetch_add(1) + 1;
    if (state_.load() == CircuitState::HALF_OPEN ||
        count >= fail_threshold_) {
      state_.store(CircuitState::OPEN);
      opened_at_ = std::chrono::steady_clock::now();
    }
  }

  bool is_open() const { return state_.load() == CircuitState::OPEN; }

  CircuitState state() const { return state_.load(); }

  void reset() {
    state_.store(CircuitState::CLOSED);
    fail_count_.store(0);
    half_open_count_.store(0);
  }

private:
  int fail_threshold_;
  int recovery_timeout_ms_;
  int half_open_max_;

  std::atomic<CircuitState> state_{CircuitState::CLOSED};
  std::atomic<int> fail_count_{0};
  std::atomic<int> half_open_count_{0};
  std::chrono::steady_clock::time_point opened_at_;
};

#endif
