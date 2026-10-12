#include "formats/pe/inspect.hpp"
#include "formats/pe/directories.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace xs::pe {
namespace {
constexpr std::array names{
    "exports", "imports",      "resources",  "exceptions", "certificates", "base_relocations",
    "debug",   "architecture", "global_ptr", "tls",        "load_config",  "bound_import",
    "iat",     "delay_import", "clr",        "reserved"};
} // namespace

nlohmann::json inspect(std::span<const std::uint8_t> bytes) {
    const Bytes r{bytes};
    if (r.number(0, 2) != 0x5a4d)
        throw std::runtime_error("Missing DOS signature");
    const auto nt = static_cast<std::size_t>(r.number(60, 4));
    r.slice(nt, 24);
    if (nt < 64 || r.number(nt, 4) != 0x4550)
        throw std::runtime_error("Invalid NT headers");
    const auto machine = r.number(nt + 4, 2);
    const auto count = r.number(nt + 6, 2);
    if (count > 96)
        throw std::runtime_error("More than 96 PE sections are not supported");
    const auto opt_size = r.number(nt + 20, 2);
    const auto opt = nt + 24;
    r.slice(opt, opt_size);
    if (opt_size < 112 || r.number(opt, 2) != 0x20b)
        throw std::runtime_error("Expected PE32+ optional header");
    const auto dir_count = r.number(opt + 108, 4);
    if (dir_count > (opt_size - 112) / 8)
        throw std::runtime_error("Directories exceed optional header");
    const auto table = opt + static_cast<std::size_t>(opt_size);
    r.slice(table, count * 40);
    const auto headers = r.number(opt + 60, 4);
    if (headers < table + count * 40 || headers > bytes.size())
        throw std::runtime_error("Invalid SizeOfHeaders");

    Image image;
    image.add(0, headers, r.slice(0, headers));
    auto sections = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        const auto s = table + 40 * i;
        std::string name;
        for (std::size_t j = 0; j < 8 && bytes[s + j]; ++j) {
            const auto c = bytes[s + j];
            name += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        const auto raw = r.number(s + 20, 4), size = r.number(s + 16, 4);
        const auto vs = r.number(s + 8, 4), start = r.number(s + 12, 4);
        const auto contents = size ? r.slice(raw, size) : Bytes{};
        image.add(start, std::max(vs, size),
                  contents.slice(0, static_cast<std::size_t>(vs ? std::min(size, vs) : size)));
        sections.push_back({{"name", name},
                            {"rva", r.number(s + 12, 4)},
                            {"virtual_size", r.number(s + 8, 4)},
                            {"raw_offset", raw},
                            {"raw_size", size},
                            {"characteristics", r.number(s + 36, 4)}});
    }
    auto dirs = nlohmann::json::array();
    for (std::size_t i = 0; i < dir_count; ++i) {
        const auto d = opt + 112 + i * 8;
        dirs.push_back({{"index", i},
                        {"name", i < names.size() ? names[i] : "unknown"},
                        {i == 4 ? "file_offset" : "rva", r.number(d, 4)},
                        {"size", r.number(d + 4, 4)}});
    }
    const auto subsystem = r.number(opt + 68, 2), base = r.number(opt + 24, 8);
    return {{"format", "PE32+"},
            {"machine", machine},
            {"architecture", machine == 0x8664   ? "x86_64"
                             : machine == 0xaa64 ? "arm64"
                                                 : "unknown"},
            {"image_base", base},
            {"entry_rva", r.number(opt + 16, 4)},
            {"subsystem", subsystem},
            {"subsystem_version", {r.number(opt + 48, 2), r.number(opt + 50, 2)}},
            {"characteristics", r.number(nt + 22, 2)},
            {"dll_characteristics", r.number(opt + 70, 2)},
            {"section_alignment", r.number(opt + 32, 4)},
            {"file_alignment", r.number(opt + 36, 4)},
            {"size_of_image", r.number(opt + 56, 4)},
            {"size_of_headers", headers},
            {"sections", sections},
            {"directory_descriptors", dirs},
            {"directory_contents",
             decode_directories(image, r.slice(opt + 112, dir_count * 8), base, machine)},
            {"platform_indicator",
             subsystem == 14 ? "Xbox subsystem; generation unknown" : "none established"},
            {"console_target_verified", false}};
}
} // namespace xs::pe
