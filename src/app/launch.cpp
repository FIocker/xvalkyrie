#include "app/launch.hpp"
#include "compatibility/modules.hpp"
#include "formats/pe/inspect.hpp"
#include "formats/xvd/identify.hpp"
#include "loader/image.hpp"
#include "platform/host/files.hpp"
#include "runtime/console.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace xs {
namespace {
std::string hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    do
        text.insert(text.begin(), digits[value & 15]);
    while (value >>= 4);
    return "0x" + text;
}
std::string names(const nlohmann::json &profile, std::string_view prefix) {
    std::string text;
    for (const auto &module : profile.at("modules")) {
        if (module.at("category").get_ref<const std::string &>().starts_with(prefix))
            text += (text.empty() ? "" : ", ") + module.at("module").get<std::string>();
    }
    return text;
}
} // namespace

LaunchResult launch_title(const std::filesystem::path &path) {
    const auto utf8 = path.u8string();
    LaunchResult result{{utf8.begin(), utf8.end()}, LaunchStatus::unreadable, {}};
    try {
        const auto prefix = host::read_prefix(path, xvd::header_size);
        if (xvd::has_magic(prefix.bytes)) {
            result.status = LaunchStatus::package_unsupported;
            const auto package = xvd::identify(prefix.bytes, prefix.file_size);
            result.detail = "This is an XVD package (content type " +
                            package.at("content_type_name").get<std::string>() + "; " +
                            package.at("encryption").get<std::string>() +
                            "). Packages cannot be verified, decrypted or mounted. Pass the "
                            "title's unpackaged executable instead.";
            return result;
        }
        const auto bytes = host::read_snapshot(path, 256 * 1024 * 1024);
        result.status = LaunchStatus::invalid_image;
        if (bytes.size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z') {
            result.detail =
                "Expected an unpackaged x64 PE32+ executable. Package loading is not supported.";
            return result;
        }
        const auto report = pe::inspect(bytes);
        if (report.at("machine") != 0x8664) {
            result.status = LaunchStatus::wrong_machine;
            result.detail = "This executable is not x86-64, the CPU architecture of Xbox One and "
                            "Xbox Series X|S.";
            return result;
        }
        const auto profile = compat::import_profile(report);
        const auto &indication = profile.at("target_indication").get_ref<const std::string &>();
        if (indication == "pc" || indication == "mixed") {
            result.status = LaunchStatus::other_target;
            result.detail = indication == "pc"
                                ? "Imports " + names(profile, "d3d_") +
                                      ", the Direct3D runtime of a Windows PC build."
                                : std::string("Imports modules of more than one target.");
            result.detail +=
                " XScarlet runs Xbox One and Xbox Series X|S builds. No guest code ran.";
            return result;
        }
        result.target = compat::console_target(profile);
        if (!result.target) {
            result.status = LaunchStatus::runtime_unavailable;
            result.detail = "Read an x64 PE32+ executable. It imports no Xbox One or Xbox Series "
                            "X|S module, so its console target is unknown. No guest code ran.";
            return result;
        }
        const std::string console(target_name(*result.target));
        auto modules =
            names(profile, *result.target == Target::durango ? "d3d_xbox_one" : "d3d_scarlett");
        if (modules.empty())
            modules = names(profile, "legacy_era");
        // no relocations = can't rebase, map it at the preferred base just to look at it
        loader::MapOptions options;
        if (!report.at("directory_contents").contains("base_relocations"))
            options.guest_base = report.at("image_base").get<std::uint64_t>();
        const loader::MappedImage image(bytes, options);
        const auto &map = image.report();
        const auto plan = runtime::bind_plan(bytes, runtime::console_registry(*result.target));
        const auto missing = plan.at("missing").get<std::size_t>();
        result.detail =
            "Imports " + modules + ", which Microsoft's tooling treats as an " + console +
            " build. Mapped " + hex(map.at("size_of_image")) + " bytes at " +
            hex(map.at("guest_base")) + " with " +
            std::to_string(map.at("relocation").at("dir64_fixups").get<std::uint64_t>()) +
            " relocations. Of " + std::to_string(plan.at("imports").size()) + " imports in " +
            std::to_string(profile.at("modules").size()) + " modules, " +
            std::to_string(plan.at("own").get<std::size_t>()) + " bind to XScarlet, " +
            std::to_string(plan.at("host").get<std::size_t>()) + " to host APIs and " +
            std::to_string(missing) + " have no implementation";
        if (!missing && runtime::execution_supported() && plan.at("imports").size()) {
            result.status = LaunchStatus::runnable;
            result.detail += ".";
            return result;
        }
        result.status = LaunchStatus::runtime_unavailable;
        std::size_t listed = 0;
        for (const auto &import : plan.at("imports")) {
            if (import.at("origin") != "missing" || listed == 4)
                continue;
            result.detail += (listed++ ? ", " : " (") + import.at("module").get<std::string>() +
                             "!" + import.at("symbol").get<std::string>();
        }
        result.detail += std::string(listed ? (missing > listed ? ", ...)" : ")") : "") +
                         ", so the entry point was not called. The " + console +
                         " runtime is not implemented. No guest code ran.";
    } catch (const std::exception &e) {
        result.detail = e.what();
    }
    return result;
}
} // namespace xs
