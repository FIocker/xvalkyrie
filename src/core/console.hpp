#pragma once
#include <memory>
#include <spdlog/logger.h>
#include <spdlog/sinks/ringbuffer_sink.h>

namespace xs {
class Console {
  public:
    Console();
    spdlog::logger &log() { return logger_; }
    auto lines() { return history_->last_formatted(); }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> history_;
    spdlog::logger logger_;
};
} // namespace xs
