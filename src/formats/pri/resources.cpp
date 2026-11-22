#include "formats/pri/resources.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace xv::pri {
namespace {
// reads inside the file or throws
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::span<const std::uint8_t> at(std::size_t offset, std::size_t count) const {
        if (offset > bytes.size() || count > bytes.size() - offset)
            throw std::runtime_error("Resource index: a field lies outside the file");
        return bytes.subspan(offset, count);
    }
    std::uint8_t u8(std::size_t offset) const { return at(offset, 1)[0]; }
    std::uint16_t u16(std::size_t offset) const {
        const auto field = at(offset, 2);
        return static_cast<std::uint16_t>(field[0] | field[1] << 8);
    }
    std::uint32_t u32(std::size_t offset) const {
        return u16(offset) | std::uint32_t{u16(offset + 2)} << 16;
    }
};
void append_utf8(std::string &text, char32_t c) {
    if (c < 0x80)
        text += static_cast<char>(c);
    else if (c < 0x800)
        text += {static_cast<char>(0xc0 | c >> 6), static_cast<char>(0x80 | (c & 0x3f))};
    else if (c < 0x10000)
        text += {static_cast<char>(0xe0 | c >> 12), static_cast<char>(0x80 | (c >> 6 & 0x3f)),
                 static_cast<char>(0x80 | (c & 0x3f))};
    else
        text += {static_cast<char>(0xf0 | c >> 18), static_cast<char>(0x80 | (c >> 12 & 0x3f)),
                 static_cast<char>(0x80 | (c >> 6 & 0x3f)), static_cast<char>(0x80 | (c & 0x3f))};
}
// UTF-16 up to `count` units or the first NUL
std::string utf16(const Reader &file, std::size_t offset, std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        char32_t c = file.u16(offset + index * 2);
        if (!c)
            break;
        if (c >= 0xd800 && c < 0xdc00 && index + 1 < count) {
            const auto low = file.u16(offset + index * 2 + 2);
            if (low >= 0xdc00 && low < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                ++index;
            }
        }
        append_utf8(text, c);
    }
    return text;
}
// a NUL-terminated string of at most `most` units
std::string wide_string(const Reader &file, std::size_t offset, std::size_t most = 4096) {
    std::size_t count = 0;
    while (count < most && file.u16(offset + count * 2))
        ++count;
    return utf16(file, offset, count);
}
std::string narrow_string(const Reader &file, std::size_t offset, std::size_t most = 4096) {
    std::string text;
    for (std::size_t index = 0; index < most; ++index) {
        const auto c = file.u8(offset + index);
        if (!c)
            break;
        // names are ASCII; anything else is kept as Latin-1
        append_utf8(text, c);
    }
    return text;
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

struct Section {
    std::string kind;  // "[mrm_res_map2_]"
    std::size_t start; // of what follows its header
};
// the language of each candidate of each decision, in the candidates' order
std::vector<std::vector<std::string>> decisions(const Reader &file, const Section &section) {
    auto at = section.start;
    const std::size_t distinct_count = file.u16(at), qualifier_count = file.u16(at + 2),
                      set_count = file.u16(at + 4), decision_count = file.u16(at + 6),
                      index_count = file.u16(at + 8);
    at += 12;
    const auto decision_table = at, set_table = at + decision_count * 4,
               qualifier_table = set_table + set_count * 4,
               distinct_table = qualifier_table + qualifier_count * 8,
               index_table = distinct_table + distinct_count * 12,
               data = index_table + index_count * 2;
    const auto indexed = [&](std::size_t index) {
        if (index >= index_count)
            throw std::runtime_error("Resource index: a decision index is out of range");
        return file.u16(index_table + index * 2);
    };
    // the language a qualifier set is for: the value of its qualifier of type 0, if any
    const auto language = [&](std::size_t set) -> std::string {
        if (set >= set_count)
            throw std::runtime_error("Resource index: a qualifier set is out of range");
        const std::size_t first = file.u16(set_table + set * 4),
                          count = file.u16(set_table + set * 4 + 2);
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t qualifier = indexed(first + index);
            if (qualifier >= qualifier_count)
                throw std::runtime_error("Resource index: a qualifier is out of range");
            const std::size_t distinct = file.u16(qualifier_table + qualifier * 8);
            if (distinct >= distinct_count)
                throw std::runtime_error("Resource index: a qualifier is out of range");
            const auto entry = distinct_table + distinct * 12;
            if (file.u16(entry + 2) == 0)
                return wide_string(file, data + std::size_t{file.u32(entry + 8)} * 2, 64);
        }
        return {};
    };
    std::vector<std::vector<std::string>> result(decision_count);
    for (std::size_t decision = 0; decision < decision_count; ++decision) {
        const std::size_t first = file.u16(decision_table + decision * 4),
                          count = file.u16(decision_table + decision * 4 + 2);
        for (std::size_t index = 0; index < count; ++index)
            result[decision].push_back(language(indexed(first + index)));
    }
    return result;
}
// the full name of each resource item, by its index: "resources/AppName"
std::map<std::size_t, std::string> names(const Reader &file, const Section &section) {
    auto at = section.start + 8;
    bool extended_names = false;
    if (section.kind == "[mrm_hschemaex]") {
        const auto kind = file.at(at, 16);
        extended_names =
            std::string_view(reinterpret_cast<const char *>(kind.data()), 13) == "[def_hnamesx]";
        at += 16;
    }
    at += 12; // version and checksum
    const std::size_t scope_count = file.u32(at), item_count = file.u32(at + 4);
    if (scope_count > 0xffff || item_count > 0xffff)
        throw std::runtime_error("Resource index: too many names");
    at += 8;
    // the two names of the schema itself, each with its NUL
    for (int name = 0; name < 2; ++name) {
        while (file.u16(at))
            at += 2;
        at += 2;
    }
    at += 18;
    const std::size_t wide_units = file.u32(at);
    at += extended_names ? 12 : 8;
    const auto entries = at, count = scope_count + item_count;
    const auto wide_data = entries + count * 12 + scope_count * 8 + item_count * 2,
               narrow_data = wide_data + wide_units * 2;
    struct Entry {
        std::size_t parent, index;
        bool scope;
        std::string name;
    };
    std::vector<Entry> list(count);
    for (std::size_t entry = 0; entry < count; ++entry) {
        const auto base = entries + entry * 12;
        const auto length = file.u8(base + 6), flags = file.u8(base + 7);
        const std::size_t offset = file.u16(base + 8) | std::size_t{flags & 0xfu} << 16;
        list[entry] = {file.u16(base), file.u16(base + 10), (flags & 0x10) != 0,
                       !length        ? std::string()
                       : flags & 0x20 ? narrow_string(file, narrow_data + offset, length)
                                      : wide_string(file, wide_data + offset * 2, length)};
    }
    std::map<std::size_t, std::string> result;
    for (const auto &entry : list) {
        if (entry.scope)
            continue;
        auto path = entry.name;
        // up through the scopes to the root, which has no name; a damaged file may loop
        auto parent = entry.parent;
        for (int depth = 0; depth < 64 && parent < count && !list[parent].name.empty(); ++depth) {
            path = list[parent].name + "/" + path;
            if (list[parent].parent == parent)
                break;
            parent = list[parent].parent;
        }
        result[entry.index] = std::move(path);
    }
    return result;
}
// the bytes of an item of a data section
std::span<const std::uint8_t> data_item(const Reader &file, const Section &section,
                                        std::size_t index) {
    const auto at = section.start + 4;
    const std::size_t string_count = file.u16(at), blob_count = file.u16(at + 2);
    const auto table = at + 8, data = table + string_count * 4 + blob_count * 8;
    if (index < string_count)
        return file.at(data + file.u16(table + index * 4), file.u16(table + index * 4 + 2));
    index -= string_count;
    if (index >= blob_count)
        throw std::runtime_error("Resource index: a data item is out of range");
    const auto entry = table + string_count * 4 + index * 8;
    return file.at(data + file.u32(entry), file.u32(entry + 4));
}
} // namespace

Strings strings(std::span<const std::uint8_t> bytes) {
    const Reader file{bytes};
    if (bytes.size() > limit || bytes.size() < 32 ||
        std::string_view(reinterpret_cast<const char *>(bytes.data()), 7) != "mrm_pri")
        throw std::runtime_error("Expected a package resource index (resources.pri)");
    const std::size_t table = file.u32(16), first = file.u32(20), count = file.u16(24);
    std::vector<Section> sections;
    for (std::size_t index = 0; index < count; ++index) {
        const auto entry = file.at(table + index * 32, 32);
        std::string kind(reinterpret_cast<const char *>(entry.data()), 16);
        kind.erase(kind.find_last_not_of(std::string_view(" \0", 2)) + 1);
        sections.push_back({kind, first + file.u32(table + index * 32 + 24) + 32});
    }
    const auto section = [&](std::size_t index, std::string_view kind) -> const Section & {
        if (index >= sections.size() || !sections[index].kind.starts_with(kind))
            throw std::runtime_error("Resource index: a section is not where it is said to be");
        return sections[index];
    };
    Strings result;
    std::size_t total = 0;
    for (const auto &map : sections) {
        if (!map.kind.starts_with("[mrm_res_map"))
            continue;
        auto at = map.start;
        const std::size_t environment = file.u16(at), schema = file.u16(at + 4),
                          schema_reference = file.u16(at + 6), decision_section = file.u16(at + 8),
                          type_count = file.u16(at + 10), group_count = file.u16(at + 12),
                          info_group_count = file.u16(at + 14), info_count = file.u32(at + 16);
        // maps too large for 16-bit tables keep theirs elsewhere: no title's names are there
        if (file.u32(at + 28))
            continue;
        const auto types = at + 32 + environment + schema_reference,
                   groups = types + type_count * 8, info_groups = groups + group_count * 4,
                   infos = info_groups + info_group_count * 4, data = infos + info_count * 4;
        const auto languages = decisions(file, section(decision_section, "[mrm_decn_info]"));
        const auto item_names = names(file, section(schema, "[mrm_hschema"));
        for (std::size_t group = 0; group < group_count; ++group) {
            const std::size_t first_item = file.u16(groups + group * 4),
                              info_group = file.u16(groups + group * 4 + 2);
            // a group of one is written as its item's own number, past the groups
            std::size_t size = 1, first_info = info_group - info_group_count;
            if (info_group < info_group_count) {
                size = file.u16(info_groups + info_group * 4);
                first_info = file.u16(info_groups + info_group * 4 + 2);
            }
            for (std::size_t item = 0; item < size; ++item) {
                if (first_info + item >= info_count)
                    throw std::runtime_error("Resource index: an item is out of range");
                const std::size_t decision = file.u16(infos + (first_info + item) * 4),
                                  candidate = file.u16(infos + (first_info + item) * 4 + 2);
                if (decision >= languages.size())
                    throw std::runtime_error("Resource index: a decision is out of range");
                const auto name = item_names.find(first_item + item);
                if (name == item_names.end())
                    continue;
                for (std::size_t index = 0; index < languages[decision].size(); ++index) {
                    const auto entry = data + (candidate + index) * 8;
                    const std::size_t type_index = file.u8(entry + 1);
                    if (type_index >= type_count)
                        throw std::runtime_error("Resource index: a value type is out of range");
                    // 0 string (UTF-16), 3 ASCII string, 4 UTF-8 string; the rest are paths
                    // and data
                    const auto type = file.u32(types + type_index * 8 + 4);
                    if (type != 0 && type != 3 && type != 4)
                        continue;
                    const auto value =
                        file.u8(entry) == 1
                            ? data_item(file, section(file.u16(entry + 6), "[mrm_dataitem]"),
                                        file.u16(entry + 4))
                            : file.at(data + file.u32(entry + 4), file.u16(entry + 2));
                    if ((total += value.size()) > limit)
                        throw std::runtime_error("Resource index: too much text");
                    std::string text;
                    if (type == 0) {
                        text = utf16(Reader{value}, 0, value.size() / 2);
                    } else {
                        text.assign(reinterpret_cast<const char *>(value.data()), value.size());
                        text.erase(std::find(text.begin(), text.end(), '\0'), text.end());
                    }
                    result[lower(name->second)].push_back({languages[decision][index], text});
                }
            }
        }
    }
    return result;
}

std::optional<std::string> resolve(const Strings &strings, std::string_view reference,
                                   std::string_view language) {
    constexpr std::string_view scheme = "ms-resource:";
    if (!lower(reference.substr(0, scheme.size())).starts_with(scheme))
        return std::nullopt;
    auto name = lower(reference.substr(scheme.size()));
    const bool rooted = name.starts_with('/');
    name.erase(0, name.find_first_not_of('/'));
    if (name.empty())
        return std::nullopt;
    if (!rooted && name.find('/') == std::string::npos)
        name = "resources/" + name;
    const auto found = strings.find(name);
    if (found == strings.end() || found->second.empty())
        return std::nullopt;
    const auto wanted = lower(language);
    const auto same = wanted.substr(0, wanted.find('-'));
    const Candidate *region = nullptr, *any = nullptr;
    for (const auto &candidate : found->second) {
        const auto has = lower(candidate.language);
        if (has == wanted)
            return candidate.value;
        if (!region && !same.empty() && has.substr(0, has.find('-')) == same)
            region = &candidate;
        if (!any && has.empty())
            any = &candidate;
    }
    return (region ? region : any ? any : &found->second.front())->value;
}
} // namespace xv::pri
