#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// resources.pri, the resource index of a package: where a manifest's "ms-resource:AppName"
// has its value. Only the strings are read. The layout is in docs/research/packages.md.
namespace xv::pri {
inline constexpr std::size_t limit = 64 * 1024 * 1024;
// one value of a resource, and the language it is for ("en-US"; empty: for any)
struct Candidate {
    std::string language, value;
};
// Every string resource, by its full name in lowercase ("resources/appname"), with its
// candidates in the order of the file. UTF-8. Paths and embedded data are left out.
// Throws on a file that is not a resource index or is damaged; never reads outside `bytes`.
using Strings = std::map<std::string, std::vector<Candidate>>;
Strings strings(std::span<const std::uint8_t> bytes);
// What a manifest's reference stands for: "ms-resource:AppName" is resources/AppName,
// "ms-resource:Scope/Name" and "ms-resource:///Scope/Name" are Scope/Name. The candidate
// for `language` if there is one, else one for the same language of another region, else
// the one for any language, else the first. Nothing if `reference` is not such a reference
// or names no string.
std::optional<std::string> resolve(const Strings &strings, std::string_view reference,
                                   std::string_view language = "en-US");
} // namespace xv::pri
