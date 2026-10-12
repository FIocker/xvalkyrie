#include "runtime/runtime.hpp"
#include "compatibility/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "guest/image.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace xs::runtime {
namespace {
std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    });
    return result;
}
} // namespace

void Registry::add(std::string_view module, std::string_view symbol, void *function) {
    if (module.empty() || symbol.empty() || !function)
        throw std::invalid_argument("Registry entries need a module, a symbol and a function");
    if (!entries_.emplace(std::pair{lower(module), std::string(symbol)}, function).second)
        throw std::invalid_argument("Duplicate registry entry");
}
void *Registry::find(std::string_view module, std::string_view symbol) const {
    const auto entry = entries_.find({lower(module), std::string(symbol)});
    return entry == entries_.end() ? nullptr : entry->second;
}

bool detail::host_policy_allows(std::string_view module, std::string_view symbol) {
    if (symbol.starts_with('#')) // can't check ordinals against the API list
        return false;
    const auto kind = compat::classify_module(module);
    const std::string_view category = kind.category;
    if (category == "crt" || category == "os_crt")
        return true;
    return (category == "api_set" || category == "core_os" || category == "win32_legacy") &&
           compat::is_game_os_api(symbol);
}

nlohmann::json bind_plan(std::span<const std::uint8_t> file, const Registry &registry) {
    using json = nlohmann::json;
    const auto contents = pe::inspect(file).at("directory_contents");
    auto imports = json::array(), opaque = json::array(), delayed = json::array();
    std::size_t own = 0, host = 0, missing = 0;
    if (contents.contains("imports")) {
        for (const auto &entry : contents.at("imports")) {
            const auto &module = entry.at("module").get_ref<const std::string &>();
            if (!entry.contains("symbols")) {
                opaque.push_back(module);
                continue;
            }
            for (const auto &item : entry.at("symbols")) {
                const auto symbol = guest::import_label(item);
                const char *origin = "missing";
                if (registry.find(module, symbol) || detail::runtime_override(module, symbol)) {
                    origin = "own";
                    ++own;
                } else if (detail::host_policy_allows(module, symbol) &&
                           detail::host_export(module, symbol)) {
                    origin = "host";
                    ++host;
                } else {
                    ++missing;
                }
                imports.push_back({{"module", module}, {"symbol", symbol}, {"origin", origin}});
            }
        }
    }
    if (contents.contains("delay_imports"))
        for (const auto &entry : contents.at("delay_imports"))
            delayed.push_back(entry.at("module"));
    return {{"execution_supported_on_this_host", execution_supported()},
            {"imports", std::move(imports)},
            {"own", own},
            {"host", host},
            {"missing", missing},
            {"modules_without_symbols", std::move(opaque)},
            {"delay_load_modules", std::move(delayed)}};
}
} // namespace xs::runtime
