#include "redis_cache.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>

RedisCache *RedisCache::GetInstance() {
  static RedisCache instance;
  return &instance;
}

void RedisCache::init(redis_pool *pool) { pool_ = pool; }

// ── 随机 TTL ────────────────────────────────────────────────────────────

int RedisCache::random_ttl(int base_ttl) const {
  // base ± 10%，最小不低于 10 秒
  static thread_local std::mt19937 rng(std::random_device{}());
  int jitter = static_cast<int>(base_ttl * 0.1);
  std::uniform_int_distribution<int> dist(-jitter, jitter);
  return std::max(10, base_ttl + dist(rng));
}

// ── 布隆过滤器预热 ──────────────────────────────────────────────────────

void RedisCache::warm_bloom(const std::vector<std::string> &keys) {
  std::lock_guard<std::mutex> lock(bloom_warm_mutex_);
  for (const auto &key : keys) {
    bloom_filter_.insert(key);
  }
  bloom_warmed_ = true;
  std::cout << "[RedisCache] 布隆过滤器预热完成: " << keys.size() << " 个键"
            << std::endl;
}

// ── 底层 Redis 操作 ─────────────────────────────────────────────────────

std::optional<std::string> RedisCache::redis_raw_get(redisContext *ctx,
                                                      const std::string &key) {
  if (!ctx) return std::nullopt;

  redisReply *reply = static_cast<redisReply *>(
      redisCommand(ctx, "GET %s", key.c_str()));

  std::optional<std::string> result;
  if (reply && reply->type == REDIS_REPLY_STRING) {
    result = std::string(reply->str, reply->len);
  }
  freeReplyObject(reply);
  return result;
}

bool RedisCache::redis_raw_set(redisContext *ctx, const std::string &key,
                                const std::string &value, int ttl) {
  if (!ctx) return false;

  redisReply *reply = static_cast<redisReply *>(
      redisCommand(ctx, "SETEX %s %d %b", key.c_str(), ttl, value.c_str(),
                   value.size()));

  bool ok = (reply && reply->type == REDIS_REPLY_STATUS &&
             strcmp(reply->str, "OK") == 0);
  freeReplyObject(reply);
  return ok;
}

bool RedisCache::redis_raw_del(redisContext *ctx, const std::string &key) {
  if (!ctx) return false;

  redisReply *reply = static_cast<redisReply *>(
      redisCommand(ctx, "DEL %s", key.c_str()));

  bool ok = (reply && reply->type == REDIS_REPLY_INTEGER);
  freeReplyObject(reply);
  return ok;
}

// ── 分布式锁 ────────────────────────────────────────────────────────────

bool RedisCache::try_lock(redisContext *ctx, const std::string &key,
                           int lock_ttl) {
  if (!ctx) return false;

  std::string lock_key = "lock:" + key;
  redisReply *reply = static_cast<redisReply *>(
      redisCommand(ctx, "SET %s 1 NX EX %d", lock_key.c_str(), lock_ttl));

  bool got = (reply && reply->type == REDIS_REPLY_STATUS &&
              strcmp(reply->str, "OK") == 0);
  freeReplyObject(reply);
  return got;
}

void RedisCache::unlock(redisContext *ctx, const std::string &key) {
  if (!ctx) return;
  std::string lock_key = "lock:" + key;
  redisReply *reply =
      static_cast<redisReply *>(redisCommand(ctx, "DEL %s", lock_key.c_str()));
  freeReplyObject(reply);
}

// ── 核心: 带三级防护的缓存读取 ──────────────────────────────────────────

std::optional<std::string>
RedisCache::get(const std::string &key,
                std::function<std::optional<std::string>()> db_query,
                int base_ttl) {
  // ═══════════════════════════════════════════════════════════════════
  // 第一层: 防缓存穿透 —— 布隆过滤器
  // ═══════════════════════════════════════════════════════════════════
  if (bloom_warmed_ && !bloom_filter_.contains(key)) {
    // 布隆判断 key 一定不存在，直接返回 nullopt，不访问 Redis / DB
    return std::nullopt;
  }

  // ═══════════════════════════════════════════════════════════════════
  // 第二层: 容错降级 —— 熔断器
  // ═══════════════════════════════════════════════════════════════════
  if (circuit_breaker_.is_open()) {
    // 熔断器打开 → 跳过 Redis，直接查 DB（降级）
    return db_query();
  }

  bool breaker_ok = circuit_breaker_.allow_request();

  // ═══════════════════════════════════════════════════════════════════
  // 首次缓存查询
  // ═══════════════════════════════════════════════════════════════════
  if (breaker_ok) {
    redisContext *ctx = nullptr;
    redisConnectionRAII conn(&ctx, pool_);

    if (ctx) {
      auto cached = redis_raw_get(ctx, key);
      if (cached.has_value()) {
        // 检查是否为穿透保护的空值标记
        if (cached.value() == "__NULL__") {
          return std::nullopt;
        }
        circuit_breaker_.on_success();
        return cached;
      }
    } else {
      circuit_breaker_.on_failure();
      return db_query(); // 获取连接失败，直接降级到 DB
    }
  }

  // ═══════════════════════════════════════════════════════════════════
  // 第三层: 防缓存击穿 —— 分布式互斥锁
  // ═══════════════════════════════════════════════════════════════════
  // 缓存未命中，尝试获取重建锁
  {
    redisContext *ctx = nullptr;
    redisConnectionRAII conn(&ctx, pool_);

    if (ctx && try_lock(ctx, key, LOCK_TTL)) {
      // —— 获得锁，负责重建缓存 ——
      // Double-check: 其他线程可能已经重建完成
      auto cached = redis_raw_get(ctx, key);
      if (cached.has_value()) {
        unlock(ctx, key);
        if (cached.value() == "__NULL__") return std::nullopt;
        circuit_breaker_.on_success();
        return cached;
      }

      // 查询数据库
      auto db_result = db_query();

      if (db_result.has_value()) {
        // 有效数据 → 写入缓存 + 插入布隆
        int ttl = random_ttl(base_ttl);
        redis_raw_set(ctx, key, db_result.value(), ttl);
        {
          std::lock_guard<std::mutex> lock(bloom_warm_mutex_);
          bloom_filter_.insert(key);
          bloom_warmed_ = true; // 首次插入即标记已预热
        }
        unlock(ctx, key);
        circuit_breaker_.on_success();
        return db_result;
      } else {
        // 空值 → 缓存短 TTL 标记，防止穿透
        redis_raw_set(ctx, key, "__NULL__", NULL_CACHE_TTL);
        unlock(ctx, key);
        circuit_breaker_.on_success();
        return std::nullopt;
      }
    }

    // —— 未获得锁，等待并重试 ——
    int retries = MAX_RETRIES;
    while (retries-- > 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(RETRY_SLEEP_MS));

      redisContext *retry_ctx = nullptr;
      redisConnectionRAII retry_conn(&retry_ctx, pool_);
      if (!retry_ctx) break;

      auto cached = redis_raw_get(retry_ctx, key);
      if (cached.has_value()) {
        if (cached.value() == "__NULL__") return std::nullopt;
        circuit_breaker_.on_success();
        return cached;
      }
    }

    // 重试耗尽，最终降级: 直接查 DB
    circuit_breaker_.on_failure();
    return db_query();
  }
}

// ── 写入缓存 ────────────────────────────────────────────────────────────

bool RedisCache::set(const std::string &key, const std::string &value,
                      int base_ttl) {
  if (circuit_breaker_.is_open()) return false;

  redisContext *ctx = nullptr;
  redisConnectionRAII conn(&ctx, pool_);
  if (!ctx) {
    circuit_breaker_.on_failure();
    return false;
  }

  int ttl = random_ttl(base_ttl);
  bool ok = redis_raw_set(ctx, key, value, ttl);
  if (ok) {
    circuit_breaker_.on_success();
  } else {
    circuit_breaker_.on_failure();
  }
  return ok;
}

// ── 删除缓存 (Cache Aside 写操作流程) ───────────────────────────────────

bool RedisCache::del(const std::string &key) {
  redisContext *ctx = nullptr;
  redisConnectionRAII conn(&ctx, pool_);
  if (!ctx) return false;

  return redis_raw_del(ctx, key);
}

// ── 批量读取 ────────────────────────────────────────────────────────────

std::vector<std::optional<std::string>>
RedisCache::mget(const std::vector<std::string> &keys,
                 std::function<std::optional<std::string>(const std::string &)>
                     db_query,
                 int base_ttl) {
  std::vector<std::optional<std::string>> results;
  results.reserve(keys.size());

  for (const auto &key : keys) {
    // 为每个 key 包装一个无参 db_query
    results.push_back(get(
        key, [&db_query, &key]() { return db_query(key); }, base_ttl));
  }
  return results;
}
