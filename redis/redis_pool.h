#ifndef REDIS_CONNECTION_POOL_H
#define REDIS_CONNECTION_POOL_H

#include <deque>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <string>
#include <unordered_map>
#include <hiredis/hiredis.h>

const int MAX_REDIS_SEM = 10000;

class redis_pool {
public:
  redisContext *GetConnection();
  bool ReleaseConnection(redisContext *conn);

  static redis_pool *GetInstance();

  void init(const std::string &host, int port, const std::string &password,
            int max_conn, int db_index = 0);

  bool is_initialized() const { return m_initialized; }

private:
  redis_pool();
  ~redis_pool();

  int m_MaxConn;
  int m_CurConn;
  int m_FreeConn;
  std::mutex lock;
  std::deque<redisContext *> connList;
  std::counting_semaphore<MAX_REDIS_SEM> semaphore_{0};

  std::string m_host;
  int m_port;
  std::string m_password;
  int m_db_index;
  bool m_initialized = false;

  // 健康检查冷却: 每60秒最多 ping 一次
  std::mutex m_health_mutex;
  std::unordered_map<redisContext *, time_t> m_last_health_check;
};

// RAII 包装器，构造时获取连接，析构时自动释放
class redisConnectionRAII {
public:
  redisConnectionRAII(redisContext **redis_conn,
                      redis_pool *connPool);
  ~redisConnectionRAII();

private:
  redisContext *conRAII;
  redis_pool *poolRAII;
};

#endif
