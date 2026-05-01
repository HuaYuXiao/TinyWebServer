#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"

using namespace std;

class Config {
public:
  Config();
  ~Config(){};

  void parse_arg(int argc, char *argv[]);

  // 端口号
  int PORT;

  // 数据库连接池数量
  int sql_num;

  // 线程池内的线程数量
  int thread_num;

  // ── Redis 配置 ──────────────────────────────
  std::string redis_host;
  int redis_port;
  std::string redis_password;
  int redis_pool_size;   // Redis 连接池大小
  int redis_db_index;    // Redis 数据库编号 (0-15)
  int cache_ttl;         // 缓存基础 TTL（秒）
};

#endif