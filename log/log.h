#ifndef LOG_H
#define LOG_H

// ──────────────────────────────────────────────────────────────────────────
// 统一日志头文件 —— 项目中所有源文件仅需 #include "log/log.h"
//
// 用法示例:
//
//   LOG_INFO("MySQL 连接池初始化完成: {} 个连接", conn_count);
//   LOG_WARN("Redis 连接超时, 回退到 MySQL 直连");
//   LOG_ERROR("query 失败: {}", mysql_error(conn));
//   LOG_DEBUG("accept 新连接: fd={}, ip={}", fd, ip);
//
// 初始化（在 main.cpp 或 webserver 启动中）:
//
//   auto &logger = app_log::Logger::instance();
//   logger.init(app_log::Level::INFO);
//   logger.add_console();
//   logger.add_file("/var/log/tinyweb/server", 100, 10);
//
// 编译期过滤: 在源文件最顶部（#include "log/log.h" 之前）定义宏:
//   #define LOG_ACTIVE_LEVEL 3   // 0=TRACE .. 5=FATAL, 此级别以下的 LOG_ 调用编译为 (void)0
//
// 设计:
//   1. 宏自动捕获 __FILE__, __LINE__, __func__
//   2. 编译期 + 运行期双重过滤，低级别日志零开销
//   3. 支持 printf 风格格式，兼容现有代码
//   4. 线程安全，各 sink 独立加锁
// ──────────────────────────────────────────────────────────────────────────

#include "logger.h"

// 编译期阈值（整数: TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4, FATAL=5）
#ifndef LOG_ACTIVE_LEVEL
#define LOG_ACTIVE_LEVEL 0
#endif

// ── 日志级别常量（宏，供 #if 使用） ──────────────────────────────────

#define LOG_LV_TRACE 0
#define LOG_LV_DEBUG 1
#define LOG_LV_INFO  2
#define LOG_LV_WARN  3
#define LOG_LV_ERROR 4
#define LOG_LV_FATAL 5

// ── 核心宏 ──────────────────────────────────────────────────────────

#define LOG_IMPL(lv, fmt, ...)                                               \
  do {                                                                       \
    if (app_log::Logger::instance().should_log(lv))                          \
      app_log::Logger::instance().log(lv, __FILE__, __LINE__, __func__, fmt, \
                                  ##__VA_ARGS__);                            \
  } while (0)

#if LOG_ACTIVE_LEVEL <= LOG_LV_TRACE
#define LOG_TRACE(fmt, ...) LOG_IMPL(app_log::Level::TRACE, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...) ((void)0)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LV_DEBUG
#define LOG_DEBUG(fmt, ...) LOG_IMPL(app_log::Level::DEBUG, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LV_INFO
#define LOG_INFO(fmt, ...) LOG_IMPL(app_log::Level::INFO, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(fmt, ...) ((void)0)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LV_WARN
#define LOG_WARN(fmt, ...) LOG_IMPL(app_log::Level::WARN, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(fmt, ...) ((void)0)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LV_ERROR
#define LOG_ERROR(fmt, ...) LOG_IMPL(app_log::Level::ERROR, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(fmt, ...) ((void)0)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LV_FATAL
#define LOG_FATAL(fmt, ...) LOG_IMPL(app_log::Level::FATAL, fmt, ##__VA_ARGS__)
#else
#define LOG_FATAL(fmt, ...) ((void)0)
#endif

#endif // LOG_H
