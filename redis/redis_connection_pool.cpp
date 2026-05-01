#include "redis_connection_pool.h"
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

redis_connection_pool::redis_connection_pool()
    : m_CurConn(0), m_FreeConn(0), m_MaxConn(0) {}

redis_connection_pool *redis_connection_pool::GetInstance() {
  static redis_connection_pool pool;
  return &pool;
}

void redis_connection_pool::init(const std::string &host, int port,
                                  const std::string &password, int max_conn,
                                  int db_index) {
  m_host = host;
  m_port = port;
  m_password = password;
  m_db_index = db_index;

  for (int i = 0; i < max_conn; ++i) {
    struct timeval timeout = {1, 500000}; // 1.5s 连接超时
    redisContext *ctx = redisConnectWithTimeout(host.c_str(), port, timeout);
    if (!ctx || ctx->err) {
      if (ctx) {
        std::cerr << "[Redis] 连接失败: " << ctx->errstr << std::endl;
        redisFree(ctx);
      } else {
        std::cerr << "[Redis] 无法分配连接上下文" << std::endl;
      }
      // 连接失败不退出，标记未初始化，后续降级到 DB 直连
      std::cerr << "[Redis] 缓存层初始化失败，将降级为纯 MySQL 模式" << std::endl;
      return;
    }

    // 认证
    if (!password.empty()) {
      redisReply *reply =
          static_cast<redisReply *>(redisCommand(ctx, "AUTH %s", password.c_str()));
      if (!reply || reply->type == REDIS_REPLY_ERROR) {
        std::cerr << "[Redis] AUTH 失败" << std::endl;
        if (reply) freeReplyObject(reply);
        redisFree(ctx);
        return;
      }
      freeReplyObject(reply);
    }

    // 选择数据库
    if (db_index > 0) {
      redisReply *reply =
          static_cast<redisReply *>(redisCommand(ctx, "SELECT %d", db_index));
      freeReplyObject(reply);
    }

    connList.emplace_back(ctx);
    ++m_FreeConn;
  }

  semaphore_.release(m_FreeConn);
  m_MaxConn = m_FreeConn;
  m_initialized = true;

  std::cout << "[Redis] 连接池初始化完成: " << m_FreeConn << " 个连接, "
            << host << ":" << port << std::endl;
}

redisContext *redis_connection_pool::GetConnection() {
  // 未初始化 → 立即返回 nullptr，不阻塞
  if (!m_initialized) return nullptr;

  semaphore_.acquire();

  redisContext *ctx = nullptr;
  {
    std::lock_guard<std::mutex> lockGuard(lock);
    ctx = connList.front();
    connList.pop_front();
    --m_FreeConn;
    ++m_CurConn;
  }

  // 健康检查（冷却 60 秒，避免每次获取都 ping）
  time_t now = time(NULL);
  bool need_check = false;
  {
    std::lock_guard<std::mutex> health_lock(m_health_mutex);
    auto it = m_last_health_check.find(ctx);
    if (it == m_last_health_check.end() || now - it->second > 60) {
      need_check = true;
    }
  }

  if (need_check) {
    redisReply *reply = static_cast<redisReply *>(redisCommand(ctx, "PING"));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      // 连接失效，重连
      {
        std::lock_guard<std::mutex> health_lock(m_health_mutex);
        m_last_health_check.erase(ctx);
      }
      if (reply) freeReplyObject(reply);
      redisFree(ctx);

      struct timeval timeout = {1, 500000};
      ctx = redisConnectWithTimeout(m_host.c_str(), m_port, timeout);
      if (!ctx || ctx->err) {
        if (ctx) {
          std::cerr << "[Redis] 重连失败: " << ctx->errstr << std::endl;
          redisFree(ctx);
        }
        ctx = nullptr;
      } else if (!m_password.empty()) {
        redisReply *auth_reply =
            static_cast<redisReply *>(redisCommand(ctx, "AUTH %s", m_password.c_str()));
        freeReplyObject(auth_reply);
      }

      if (!ctx) {
        std::lock_guard<std::mutex> lockGuard(lock);
        --m_CurConn;
        semaphore_.release();
        return nullptr;
      }
    } else {
      freeReplyObject(reply);
    }
  }

  if (ctx) {
    std::lock_guard<std::mutex> health_lock(m_health_mutex);
    m_last_health_check[ctx] = now;
  }

  return ctx;
}

bool redis_connection_pool::ReleaseConnection(redisContext *ctx) {
  if (!ctx) return false;

  {
    std::lock_guard<std::mutex> lockGuard(lock);
    connList.emplace_back(ctx);
    ++m_FreeConn;
    --m_CurConn;
  }
  semaphore_.release();
  return true;
}

redis_connection_pool::~redis_connection_pool() {
  {
    std::lock_guard<std::mutex> lockGuard(lock);
    for (redisContext *ctx : connList) {
      redisFree(ctx);
    }
    m_CurConn = 0;
    m_FreeConn = 0;
    connList.clear();
  }
}

redisConnectionRAII::redisConnectionRAII(redisContext **redis_conn,
                                          redis_connection_pool *connPool) {
  *redis_conn = connPool->GetConnection();
  conRAII = *redis_conn;
  poolRAII = connPool;
}

redisConnectionRAII::~redisConnectionRAII() {
  poolRAII->ReleaseConnection(conRAII);
}
