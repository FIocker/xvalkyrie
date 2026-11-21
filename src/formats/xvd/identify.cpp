#include "formats/xvd/identify.hpp"
#include "formats/pe/image.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <string>

// Layout from community notes on the container format.
// Not checked against a real Series package yet.
namespace xv::xvd {
namespace {
using json = nlohmann::json;
constexpr std::string_view magic = "msft-xvd";
constexpr std::array content_types{"Data",
                                   "Title",
                                   "SystemOS",
                                   "EraOS",
                                   "Scratch",
                                   "ResetData",
                                   "Application",
                                   "HostOS",
                                   "X360STFS",
                                   "X360FATX",
                                   "X360GDFX",
                                   "Updater",
                                   "OfflineUpdater",
                                   "Template",
                                   "MteHost",
                                   "MteApp",
                                   "MteTitle",
                                   "MteEraOS",
                                   "EraTools",
                                   "SystemTools",
                                   "SystemAux",
                                   "AcousticModel",
                                   "SystemCodecsVolume",
                                   "QasltPackage",
                                   "AppDlc",
                                   "TitleDlc",
                                   "UniversalDlc",
                                   "SystemDataVolume",
                                   "TestVolume",
                                   "HardwareTestVolume",
                                   "KioskContent",
                                   "DevPackage",
                                   "HostProfiler",
                                   "Roamable",
                                   "ThinProvisioned",
                                   "StreamingOnlySRA",
                                   "StreamingOnlyERA",
                                   "StreamingOnlyHost",
                                   "QuickResume",
                                   "HostData"};
constexpr std::array flag_names{"ReadOnly",         "EncryptionDisabled", "DataIntegrityDisabled",
                                "LegacySectorSize", "ResiliencyEnabled",  "SraReadOnly",
                                "RegionIdInXts",    "EraSpecific"};
// bitmask of content types that are XVCs
constexpr std::uint32_t xvc_types = 0x07018042;

std::string hex(pe::Bytes field) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : field.bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}
json version(pe::Bytes header, std::size_t pos) {
    return {header.number(pos, 2), header.number(pos + 2, 2), header.number(pos + 4, 2),
            header.number(pos + 6, 2)};
}
} // namespace

bool has_magic(std::span<const std::uint8_t> prefix) {
    return prefix.size() >= 0x208 && std::equal(magic.begin(), magic.end(), prefix.begin() + 0x200);
}

json identify(std::span<const std::uint8_t> prefix, std::uint64_t file_size) {
    if (!has_magic(prefix))
        throw std::runtime_error("Missing msft-xvd tag at offset 0x200");
    if (prefix.size() < header_size)
        throw std::runtime_error("Truncated XVD header");
    const pe::Bytes h{prefix.first(header_size)};
    const auto flags = h.number(0x208, 4), type = h.number(0x284, 4);
    auto set = json::array();
    for (std::size_t bit = 0; bit < flag_names.size(); ++bit)
        if (flags >> bit & 1)
            set.push_back(flag_names[bit]);
    const auto encrypted = !(flags & 2), hashed = !(flags & 4);
    const auto signature = h.slice(0x20, 0x1e0).bytes;
    std::string sandbox;
    for (const auto c : h.slice(0x38c, 16).bytes) {
        if (!c)
            break;
        sandbox += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    const auto platforms = h.number(0x471, 1);
    return {
        {"format", "XVD"},
        {"file_size", file_size},
        {"format_version", h.number(0x20c, 4)},
        {"volume_flags", flags},
        {"volume_flag_names", std::move(set)},
        {"unnamed_volume_flag_bits", flags >> flag_names.size() << flag_names.size()},
        {"drive_size", h.number(0x218, 8)},
        {"vduid_bytes", hex(h.slice(0x220, 16))},
        {"xvd_type", h.number(0x280, 4)},
        {"content_type", type},
        {"content_type_name", type < content_types.size() ? content_types[type] : "unknown"},
        {"xvc_content_type", type < 32 && (xvc_types >> type & 1) != 0},
        {"embedded_xvd_length", h.number(0x288, 4)},
        {"user_data_length", h.number(0x28c, 4)},
        {"xvc_info_length", h.number(0x290, 4)},
        {"dynamic_header_length", h.number(0x294, 4)},
        {"block_size", h.number(0x298, 4)},
        {"sandbox_id", sandbox},
        {"product_id_bytes", hex(h.slice(0x39c, 16))},
        {"pduid_bytes", hex(h.slice(0x3ac, 16))},
        {"package_version_words", version(h, 0x3bc)},
        {"required_system_version_words", version(h, 0x494)},
        // documented by community notes on the container format
        {"platform_mask",
         {{"raw", platforms},
          {"pc", (platforms & 1) != 0},
          {"gen8", (platforms & 2) != 0},
          {"gen9", (platforms & 4) != 0},
          {"confidence", "single third-party source; unverified"}}},
        {"signature_area", std::any_of(signature.begin(), signature.end(), [](auto b) { return b; })
                               ? "present; not verified"
                               : "empty"},
        {"data_integrity", hashed ? "hash tree declared; not verified" : "disabled by volume flag"},
        {"encryption", encrypted ? "data regions declared encrypted; no decryption support"
                                 : "disabled by volume flag; contents still not read"},
        {"scarlett_layout_verified", false},
        {"mount_supported", false}};
}
} // namespace xv::xvd
