#ifndef LOG_SINK_H
#define LOG_SINK_H

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "level.h"

namespace app_log {

struct LogMessage {
  Level level;
  std::string timestamp;
  std::string thread_name;
  std::string file;
  int line;
  std::string func;
  std::string text;
};

// ── 抽象 Sink ───────────────────────────────────────────────────────

class Sink {
public:
  virtual ~Sink() = default;
  virtual void write(const LogMessage &msg) = 0;
  virtual void flush() {}
  virtual std::string name() const = 0;
};

// ── 控制台 Sink（带颜色） ────────────────────────────────────────────

class ConsoleSink : public Sink {
public:
  ConsoleSink(bool color = true) : color_(color) {}

  void write(const LogMessage &msg) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (color_) fprintf(stderr, "%s", level_color(msg.level));
#ifdef LOG_NO_TIMESTAMP
    fprintf(stderr, "[%s] ", level_name(msg.level));
#else
    fprintf(stderr, "%s [%s] ", msg.timestamp.c_str(), level_name(msg.level));
#endif
    if (!msg.thread_name.empty())
      fprintf(stderr, "[%s] ", msg.thread_name.c_str());
    if (color_) fprintf(stderr, "\033[0m");
    fprintf(stderr, "%s", msg.text.c_str());
    if (!msg.text.empty() && msg.text.back() != '\n')
      fprintf(stderr, "\n");
    fflush(stderr);
  }

  std::string name() const override { return "console"; }

private:
  bool color_;
  std::mutex mutex_;
};

// ── 文件 Sink（按天滚动） ────────────────────────────────────────────

class FileSink : public Sink {
public:
  FileSink(const std::string &base_path, size_t max_size_mb = 100,
           int max_files = 10)
      : base_path_(base_path), max_size_(max_size_mb * 1024 * 1024),
        max_files_(max_files) {
    open();
  }

  void write(const LogMessage &msg) override {
    std::lock_guard<std::mutex> lock(mutex_);

    // 日期变更 → 滚动
    auto today = current_date();
    if (today != current_date_str_) {
      current_date_str_ = today;
      file_.close();
      open();
    }

    // 大小超限 → 滚动
    if (file_.is_open() && file_.tellp() > static_cast<long>(max_size_)) {
      file_.close();
      rotate();
      open();
    }

    if (file_.is_open()) {
      file_ << msg.timestamp << " [" << level_name(msg.level) << "] ";
      if (!msg.thread_name.empty())
        file_ << "[" << msg.thread_name << "] ";
      file_ << msg.file << ":" << msg.line << " " << msg.func << " — "
            << msg.text;
      if (!msg.text.empty() && msg.text.back() != '\n')
        file_ << "\n";
      file_.flush();
    }
  }

  std::string name() const override { return "file:" + base_path_; }

private:
  static std::string current_date() {
    time_t now = time(nullptr);
    struct tm tm_buf;
    char buf[16];
    strftime(buf, sizeof(buf), "%Y%m%d",
             localtime_r(&now, &tm_buf));
    return buf;
  }

  void open() {
    current_date_str_ = current_date();
    auto path = base_path_ + "/" + current_date_str_ + ".log";
    file_.open(path, std::ios::app);
  }

  void rotate() {
    for (int i = max_files_ - 1; i >= 0; --i) {
      std::string old_path, new_path;
      if (i == 0)
        old_path = base_path_ + "/" + current_date_str_ + ".log";
      else
        old_path = base_path_ + "/" + current_date_str_ + "." +
                   std::to_string(i) + ".log";
      new_path = base_path_ + "/" + current_date_str_ + "." +
                 std::to_string(i + 1) + ".log";
      std::rename(old_path.c_str(), new_path.c_str());
    }
  }

  std::string base_path_;
  size_t max_size_;
  int max_files_;
  std::string current_date_str_;
  std::ofstream file_;
  std::mutex mutex_;
};

} // namespace app_log

#endif
