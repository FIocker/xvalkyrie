#include "core/log.hpp"
#include <spdlog/sinks/stdout_sinks.h>

namespace xv {
Log::Log()
    : history_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128)),
      logger_("xvalkyrie", {history_, std::make_shared<spdlog::sinks::stderr_sink_mt>()}) {
    logger_.set_pattern("%T  [%l]  %n  %v");
}
} // namespace xv
