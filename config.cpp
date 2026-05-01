#include "config.h"

Config::Config() {
  // 端口号,默认 9006
  PORT = 9006;

  // 数据库连接池数量,默认 100
  sql_num = 100;

  // 线程池内的线程数量,默认 64
  thread_num = 64;

  // Redis 默认配置
  redis_host = "127.0.0.1";
  redis_port = 6379;
  redis_password = "";
  redis_pool_size = 16;
  redis_db_index = 0;
  cache_ttl = 3600;
}

void Config::parse_arg(int argc, char *argv[]) {
  int opt;
  const char *str = "p:s:t:r:";
  while ((opt = getopt(argc, argv, str)) != -1) {
    switch (opt) {
    case 'p': {
      PORT = atoi(optarg);
      break;
    }
    case 's': {
      sql_num = atoi(optarg);
      break;
    }
    case 't': {
      thread_num = atoi(optarg);
      break;
    }
    case 'r': {
      redis_pool_size = atoi(optarg);
      break;
    }
    default:
      break;
    }
  }
}