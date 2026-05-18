#ifndef LOG_LOGGER_H
#define LOG_LOGGER_H

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "level.h"
#include "sink.h"

namespace app_log {

class Logger {
public:
  static Logger &instance() {
    static Logger logger;
    return logger;
  }

  // ── 初始化 ─────────────────────────────────────────────────────

  void init(Level min_level = Level::INFO, bool async = true) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = min_level;
    async_ = async;
  }

  // ── Sink 管理 ──────────────────────────────────────────────────

  void add_sink(std::unique_ptr<Sink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
  }

  // 便捷: 添加控制台输出
  void add_console(bool color = true) {
    add_sink(std::make_unique<ConsoleSink>(color));
  }

  // 便捷: 添加文件输出
  void add_file(const std::string &path, size_t max_mb = 100,
                int max_files = 10) {
    add_sink(std::make_unique<FileSink>(path, max_mb, max_files));
  }

  // ── 源文件路径处理 ─────────────────────────────────────────────

  void set_source_root(const std::string &root) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_root_ = root;
    if (!source_root_.empty() && source_root_.back() != '/')
      source_root_ += '/';
  }

  // ── 级别控制 ───────────────────────────────────────────────────

  void set_level(Level lv) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = lv;
  }

  Level level() const { return min_level_.load(); }

  bool should_log(Level lv) const { return lv >= min_level_.load(); }

  // ── 核心写入 ───────────────────────────────────────────────────

  void log(Level lv, const char *file, int line, const char *func,
           const char *fmt, ...) {
    if (!should_log(lv)) return;

    // 格式化用户消息
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;

    // 去除绝对路径前缀
    const char *display = file;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!source_root_.empty()) {
        std::string_view sv(file);
        if (sv.starts_with(source_root_))
          display = file + source_root_.size();
      }
    }

    LogMessage msg{
        .level       = lv,
        .timestamp   = timestamp_now(),
        .thread_name = "",
        .file        = display,
        .line        = line,
        .func        = func,
        .text        = std::string(buf, std::min(n, (int)sizeof(buf) - 1)),
    };

    write_to_sinks(msg);
  }

  void flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &sink : sinks_)
      sink->flush();
  }

private:
  Logger() = default;

  static std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms.count());
    return buf;
  }

  void write_to_sinks(const LogMessage &msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &sink : sinks_)
      sink->write(msg);
  }

  std::atomic<Level> min_level_{Level::INFO};
  bool async_ = false;
  std::string source_root_;
  std::vector<std::unique_ptr<Sink>> sinks_;
  std::mutex mutex_;
};

} // namespace app_log

#endif
