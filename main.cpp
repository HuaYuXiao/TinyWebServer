#include "config.h"
#include "log/log.h"

int main(int argc, char *argv[]) {
  // 日志初始化
  auto &logger = app_log::Logger::instance();
  logger.init(app_log::Level::INFO);
  logger.set_source_root("/home/user/TinyWebServer/");
  logger.add_console(true);
  logger.add_file("/home/user/TinyWebServer/build", 100, 10);

  LOG_INFO("========== TinyWebServer starting ==========");

  string user = "user";
  string passwd = "123456";
  string databasename = "server";

  // 命令行解析
  Config config;
  config.parse_arg(argc, argv);

  WebServer server;

  // 初始化
  server.init(config.PORT, user, passwd, databasename, config.sql_num,
              config.thread_num, config.auth_enabled);

  LOG_INFO("Config: port=%d, sql_pool=%d, threads=%d, redis_pool=%d, auth=%s",
           config.PORT, config.sql_num, config.thread_num,
           config.redis_pool_size, config.auth_enabled ? "on" : "off");

  // 数据库
  server.init_mysql_pool();

  // 线程池
  server.init_thread_pool();

  // Redis
  server.init_redis_pool();

  // 监听
  server.eventListen();

  // 运行
  server.eventLoop();

  return 0;
}