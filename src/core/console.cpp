#include "core/console.hpp"
#include <spdlog/sinks/stdout_sinks.h>

namespace xs {
Console::Console()
    : history_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128)),
      logger_("xscarlet", {history_, std::make_shared<spdlog::sinks::stderr_sink_mt>()}) {
    logger_.set_pattern("%T  [%l]  %n  %v");
}
} // namespace xs
