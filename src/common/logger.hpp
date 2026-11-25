#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace streamer {

enum class LogLevel { Info, Warn, Error };

/// Thread-safe UTC-timestamped structured logger for container and CLI
/// telemetry.
class Logger {
public:
  static Logger &instance() {
    static Logger logger;
    return logger;
  }

  void log(LogLevel level, const std::string &component,
           const std::string &message) {
    const auto now = std::chrono::system_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;
    const std::time_t now_t = std::chrono::system_clock::to_time_t(now);

    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now_t);
#else
    gmtime_r(&now_t, &utc_tm);
#endif

    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                  utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
                  static_cast<int>(now_ms.count()));

    const char *level_str = "INFO ";
    std::ostream *out = &std::cout;
    if (level == LogLevel::Warn) {
      level_str = "WARN ";
      out = &std::cerr;
    } else if (level == LogLevel::Error) {
      level_str = "ERROR";
      out = &std::cerr;
    }

    std::lock_guard<std::mutex> lock(mu_);
    (*out) << ts_buf << " [" << level_str << "] [" << component << "] "
           << message << std::endl;
  }

private:
  Logger() = default;
  std::mutex mu_;
};

inline void log_info(const std::string &component, const std::string &msg) {
  Logger::instance().log(LogLevel::Info, component, msg);
}

inline void log_warn(const std::string &component, const std::string &msg) {
  Logger::instance().log(LogLevel::Warn, component, msg);
}

inline void log_error(const std::string &component, const std::string &msg) {
  Logger::instance().log(LogLevel::Error, component, msg);
}

} //< namespace streamer
