#include "compatibility/assessment.hpp"
#include "compatibility/modules.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace xs::compat {
namespace {
using json = nlohmann::json;
struct Reference {
    std::string_view name, url;
};
// Public source names only; none establish a console DLL or ordinal binding.
constexpr std::array references{
    Reference{"XGameRuntimeInitialize",
              "https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgameruntimeinit/"
              "functions/xgameruntimeinitialize?view=gdk-2604"},
    Reference{"XGameRuntimeUninitialize",
              "https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xgameruntimeinit/"
              "functions/xgameruntimeuninitialize?view=gdk-2510"},
    Reference{"D3D12XboxCreateDevice",
              "https://github.com/microsoft/Xbox-GDK-Samples/blob/"
              "b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Samples/IntroGraphics/SimpleTriangle/"
              "DeviceResources.cpp#L100"}};
const Reference *lookup(std::string_view symbol) {
    for (const auto &ref : references)
        if (symbol == ref.name)
            return &ref;
    return nullptr;
}
} // namespace

json assess(const json &pe_report) {
    const auto &contents = pe_report.at("directory_contents");
    const auto table = [&](const char *key) {
        auto imports = json::array();
        if (!contents.contains(key))
            return imports;
        for (const auto &entry : contents.at(key)) {
            const auto &module = entry.at("module").get_ref<const std::string &>();
            const auto kind = classify_module(module);
            json item{{"module", module}, {"category", kind.category}};
            if (!entry.contains("symbols")) {
                item["inspection_status"] = entry.at("status");
                item["symbols"] = nullptr;
            } else {
                auto symbols = json::array();
                for (const auto &symbol : entry.at("symbols")) {
                    json row = symbol;
                    const auto *name = symbol.contains("name")
                                           ? &symbol.at("name").get_ref<const std::string &>()
                                           : nullptr;
                    row["evidence_match"] = name && lookup(*name) ? "source_name_only"
                                            : name && kind.os_surface && is_game_os_api(*name)
                                                ? "game_os_api_name"
                                                : "unknown";
                    row["implementation"] = "unsupported";
                    symbols.push_back(std::move(row));
                }
                item["symbols"] = std::move(symbols);
            }
            imports.push_back(std::move(item));
        }
        return imports;
    };
    auto refs = json::object();
    for (const auto &ref : references)
        refs[ref.name] = ref.url;
    auto profile = import_profile(pe_report);
    const auto target = console_target(profile);
    return {{"target", target ? target_id(*target) : "unknown"},
            {"source_references", std::move(refs)},
            {"format", pe_report.at("format")},
            {"architecture", pe_report.at("architecture")},
            {"within_x64_scope", pe_report.at("machine") == 0x8664},
            {"platform_indicator", pe_report.at("platform_indicator")},
            {"standard_imports", table("imports")},
            {"delay_imports", table("delay_imports")},
            {"import_profile", std::move(profile)},
            {"console_target_verified", false},
            {"execution_supported", false}};
}

json compare_declaration(const json &pe, const json &config, std::string_view name) {
    const json *entry = nullptr;
    for (const auto &candidate : config.at("executables")) {
        if (candidate.at("name").get_ref<const std::string &>() == name) {
            if (entry)
                throw std::invalid_argument("Ambiguous executable Name in game config");
            entry = &candidate;
        }
    }
    if (!entry)
        throw std::invalid_argument("Requested executable Name not found in game config");
    const auto &architecture = entry->at("declared_architecture");
    const auto &machine = pe.at("machine");
    const auto arch_matches = (architecture == "x64" && machine == 0x8664) ||
                              (architecture == "ARM64" && machine == 0xaa64);
    const auto target = console_target(import_profile(pe));
    const auto &family = entry->at("declared_device_family");
    return {{"association", "user_supplied_entry_name"},
            {"declaration", *entry},
            {"family_vs_target", entry->at("device_family_status") != "documented_value" ? "unknown"
                                 : family == "PC" ? "conflict"
                                 : !target        ? "unknown"
                                 : family == (*target == Target::durango ? "XboxOne" : "Scarlett")
                                     ? "consistent"
                                     : "conflict"},
            {"declared_architecture_vs_pe", architecture != "x64" && architecture != "ARM64"
                                                ? "unknown"
                                            : arch_matches ? "consistent"
                                                           : "conflict"}};
}
} // namespace xs::compat
