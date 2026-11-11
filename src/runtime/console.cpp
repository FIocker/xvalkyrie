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
bool windows_app(std::span<const std::uint8_t> file) {
    const auto report = pe::inspect(file, pe::Detail::light);
    constexpr std::uint64_t app_container = 0x1000;
    return !analysis::console_target(analysis::import_profile(report)) &&
           (report.at("dll_characteristics").get<std::uint64_t>() & app_container);
}
Registry title_registry(std::span<const std::uint8_t> file) {
    const auto target =
        analysis::console_target(analysis::import_profile(pe::inspect(file, pe::Detail::light)));
    if (target)
        return console_registry(*target);
    Registry registry;
    if (windows_app(file))
        registry.open_to_host();
    return registry;
}
} // namespace xs::runtime
