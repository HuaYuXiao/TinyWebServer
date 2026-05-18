#ifndef LOG_LEVEL_H
#define LOG_LEVEL_H

#include <cstdint>

namespace app_log {

enum class Level : uint8_t {
  TRACE = 0,
  DEBUG = 1,
  INFO  = 2,
  WARN  = 3,
  ERROR = 4,
  FATAL = 5,
  OFF   = 6
};

inline const char* level_name(Level lv) {
  switch (lv) {
  case Level::TRACE: return "TRACE";
  case Level::DEBUG: return "DEBUG";
  case Level::INFO:  return "INFO";
  case Level::WARN:  return "WARN";
  case Level::ERROR: return "ERROR";
  case Level::FATAL: return "FATAL";
  default:           return "????";
  }
}

inline const char* level_color(Level lv) {
  switch (lv) {
  case Level::TRACE: return "\033[90m";     // grey
  case Level::DEBUG: return "\033[36m";     // cyan
  case Level::INFO:  return "\033[32m";     // green
  case Level::WARN:  return "\033[33m";     // yellow
  case Level::ERROR: return "\033[31m";     // red
  case Level::FATAL: return "\033[35m";     // magenta
  default:           return "\033[0m";
  }
}

} // namespace app_log

#endif
