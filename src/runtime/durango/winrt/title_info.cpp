#include "runtime/durango/winrt/title_info.hpp"
#include "formats/appx/manifest.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace xv::runtime::durango::winrt {
namespace {
struct Info {
    std::wstring title_id, service_config_id;
};
std::wstring wide(const std::string &text) { return {text.begin(), text.end()}; } // ASCII
Info read() {
    Info info;
    try {
        const auto manifest = appx::inspect_manifest(host::read_snapshot(
            package_directory(std::filesystem::current_path()) / "AppxManifest.xml",
            appx::manifest_limit));
        if (manifest.at("xbox_live_title_id").is_string())
            info.title_id = std::to_wstring(
                std::stoul(manifest.at("xbox_live_title_id").get<std::string>(), nullptr, 16));
        if (manifest.at("service_config_id").is_string())
            info.service_config_id = wide(manifest.at("service_config_id").get<std::string>());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] no Xbox Live identity from the manifest: %s\n", e.what());
    }
    std::fprintf(stderr, "[Runtime] Xbox Live title %ls, service configuration %ls\n",
                 info.title_id.c_str(), info.service_config_id.c_str());
    return info;
}
} // namespace

std::wstring title_info(std::wstring_view key) {
    static const auto info = read();
    if (key == L"title_id")
        return info.title_id;
    if (key == L"service_config_id")
        return info.service_config_id;
    if (key == L"sandbox_id")
        return L"RETAIL";
    return {};
}
} // namespace xv::runtime::durango::winrt
