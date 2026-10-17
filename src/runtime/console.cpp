#include "runtime/console.hpp"
#include "analysis/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "runtime/durango/runtime.hpp"
#include "runtime/scarlett/runtime.hpp"
#include <nlohmann/json.hpp>

namespace xs::runtime {
Registry console_registry(Target target) {
    Registry registry;
    if (target == Target::durango)
        durango::install(registry);
    else
        scarlett::install(registry);
    return registry;
}
Registry title_registry(std::span<const std::uint8_t> file) {
    const auto target = analysis::console_target(analysis::import_profile(pe::inspect(file)));
    return target ? console_registry(*target) : Registry{};
}
} // namespace xs::runtime
