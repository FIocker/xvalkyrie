#include "formats/pe/directories.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace xs::pe {
namespace {
using json = nlohmann::json;

struct Budget {
    std::size_t records = 4 * 1024 * 1024;
    std::size_t characters = 16 * 1024 * 1024;
    void consume(std::uint64_t n = 1) {
        if (n > records)
            throw std::runtime_error("Directory report exceeds record limit");
        records -= static_cast<std::size_t>(n);
    }
    std::string text(Bytes data) {
        const auto count = std::min({data.bytes.size(), characters + 1, std::size_t{4097}});
        const auto bytes = data.bytes.first(count);
        const auto end = std::find(bytes.begin(), bytes.end(), 0);
        if (end == bytes.end())
            throw std::runtime_error("Unterminated or oversized PE string");
        if (!std::all_of(bytes.begin(), end, [](auto c) { return c < 128; }))
            throw std::runtime_error("Non-ASCII PE string");
        std::string value(bytes.begin(), end);
        characters -= value.size();
        return value;
    }
};

json symbols(const Image &image, std::uint64_t table_rva, Budget &budget) {
    const auto thunks = image.at(table_rva);
    auto result = json::array();
    for (std::size_t slot = 0;; slot += 8) {
        const auto thunk = thunks.number(slot, 8);
        if (!thunk)
            return result;
        budget.consume();
        if ((thunk >> 63) != 0) {
            if ((thunk & 0x7fffffffffff0000ULL) != 0)
                throw std::runtime_error("Reserved ordinal thunk bits are set");
            result.push_back({{"ordinal", thunk & 0xffff}});
        } else {
            if (thunk > 0x7fffffff)
                throw std::runtime_error("Reserved import name thunk bits are set");
            const auto hint_name = image.at(thunk);
            const auto hint = hint_name.number(0, 2);
            result.push_back({{"hint", hint}, {"name", budget.text({hint_name.bytes.subspan(2)})}});
        }
    }
}
bool terminator(Bytes d) {
    return std::all_of(d.bytes.begin(), d.bytes.end(), [](auto c) { return c == 0; });
}

json imports(const Image &image, Bytes data, Budget &budget) {
    auto result = json::array();
    for (std::size_t pos = 0; pos < data.bytes.size(); pos += 20) {
        const auto d = data.slice(pos, 20);
        if (terminator(d))
            return result;
        budget.consume();
        const auto lookup = d.number(0, 4), name = d.number(12, 4), iat = d.number(16, 4);
        if (!name || !iat)
            throw std::runtime_error("Missing import name or address table");
        json module{{"module", budget.text(image.at(name))}, {"iat_rva", iat}};
        if (!lookup && d.number(4, 4) != 0)
            module["status"] = "bound IAT without lookup table; symbols unknown";
        else
            module["symbols"] = symbols(image, lookup ? lookup : iat, budget);
        result.push_back(std::move(module));
    }
    throw std::runtime_error("Missing import descriptor terminator");
}

// Only RVA-based descriptors (attribute bit 0), which is what x64 linkers emit.
json delay_imports(const Image &image, Bytes data, Budget &budget) {
    auto result = json::array();
    for (std::size_t pos = 0; pos < data.bytes.size(); pos += 32) {
        const auto d = data.slice(pos, 32);
        if (terminator(d))
            return result;
        budget.consume();
        const auto name = d.number(4, 4), iat = d.number(12, 4), names = d.number(16, 4);
        if (!(d.number(0, 4) & 1))
            throw std::runtime_error("VA-based delay import descriptors are not supported");
        if (!name || !iat)
            throw std::runtime_error("Missing delay import name or address table");
        json module{{"module", budget.text(image.at(name))}, {"iat_rva", iat}};
        if (names)
            module["symbols"] = symbols(image, names, budget);
        else
            module["status"] = "delay descriptor without name table; symbols unknown";
        result.push_back(std::move(module));
    }
    throw std::runtime_error("Missing delay import descriptor terminator");
}

Bytes table(const Image &image, std::uint64_t rva, std::uint64_t count, std::size_t stride) {
    if (!count)
        return {};
    if (!rva)
        throw std::runtime_error("Missing export table RVA");
    // The caller charged count against the report budget before multiplying.
    return image.at(rva).slice(0, static_cast<std::size_t>(count) * stride);
}
json exports(const Image &image, Bytes data, std::uint64_t start, Budget &budget) {
    const auto d = data.slice(0, 40);
    const auto base = d.number(16, 4), functions = d.number(20, 4), names = d.number(24, 4);
    budget.consume(functions + names);
    const auto addresses = table(image, d.number(28, 4), functions, 4);
    const auto strings = table(image, d.number(32, 4), names, 4);
    const auto ordinals = table(image, d.number(36, 4), names, 2);
    auto entries = json::array();
    for (std::size_t i = 0; i < functions; ++i) {
        const auto target = addresses.number(4 * i, 4);
        json symbol{{"ordinal", base + i}, {"rva", target}, {"names", json::array()}};
        if (!target)
            symbol["unused"] = true;
        else if (target >= start && target - start < data.bytes.size())
            symbol["forwarder"] =
                budget.text({data.bytes.subspan(static_cast<std::size_t>(target - start))});
        entries.push_back(std::move(symbol));
    }
    for (std::size_t i = 0; i < names; ++i) {
        const auto index = ordinals.number(2 * i, 2);
        if (index >= functions)
            throw std::runtime_error("Export ordinal index exceeds address table");
        if (!strings.number(4 * i, 4))
            throw std::runtime_error("Missing export name RVA");
        entries[static_cast<std::size_t>(index)]["names"].push_back(
            budget.text(image.at(strings.number(4 * i, 4))));
    }
    if (!d.number(12, 4))
        throw std::runtime_error("Missing export module name RVA");
    return {{"module", budget.text(image.at(d.number(12, 4)))}, {"symbols", std::move(entries)}};
}

json relocations(Bytes data, Budget &budget) {
    auto blocks = json::array();
    for (std::size_t pos = 0; pos < data.bytes.size();) {
        const auto page = data.number(pos, 4), size = data.number(pos + 4, 4);
        if (size < 8 || size % 2 || pos % 4)
            throw std::runtime_error("Invalid relocation block layout");
        const auto block = data.slice(pos, static_cast<std::size_t>(size));
        budget.consume(1 + (size - 8) / 2);
        auto entries = json::array();
        for (std::size_t i = 8; i < size; i += 2) {
            const auto word = block.number(i, 2), type = word >> 12;
            json item{{"type", type}, {"offset", word & 0xfff}};
            if (type == 4) {
                // HIGHADJ occupies two slots; the second word is data, not a fixup.
                i += 2;
                item["adjustment_word"] = block.number(i, 2);
            }
            entries.push_back(std::move(item));
        }
        blocks.push_back({{"page_rva", page}, {"entries", std::move(entries)}});
        pos += static_cast<std::size_t>(size);
    }
    return blocks;
}

json tls(const Image &image, Bytes data, std::uint64_t base, Budget &budget) {
    const auto d = data.slice(0, 40);
    const auto va = d.number(24, 8);
    auto callbacks = json::array();
    if (va) {
        if (va < base)
            throw std::runtime_error("TLS callback table VA precedes image base");
        const auto list = image.at(va - base);
        for (std::size_t i = 0;; i += 8) {
            const auto address = list.number(i, 8);
            if (!address)
                break;
            budget.consume();
            callbacks.push_back(address);
        }
    }
    return {{"template_start_va", d.number(0, 8)},  {"template_end_va", d.number(8, 8)},
            {"index_va", d.number(16, 8)},          {"callback_table_va", va},
            {"callbacks_va", std::move(callbacks)}, {"zero_fill", d.number(32, 4)},
            {"characteristics", d.number(36, 4)}};
}

json exceptions(Bytes data, std::uint64_t machine, Budget &budget) {
    if (machine != 0x8664)
        return {{"status", "unsupported machine-specific exception layout"}};
    if (data.bytes.size() % 12)
        throw std::runtime_error("Partial AMD64 runtime function record");
    budget.consume(data.bytes.size() / 12);
    auto entries = json::array();
    for (std::size_t i = 0; i < data.bytes.size(); i += 12) {
        entries.push_back({{"begin_rva", data.number(i, 4)},
                           {"end_rva", data.number(i + 4, 4)},
                           {"unwind_info_rva", data.number(i + 8, 4)}});
    }
    return entries;
}
} // namespace

nlohmann::json decode_directories(const Image &image, Bytes descriptors, std::uint64_t base,
                                  std::uint64_t machine) {
    Budget budget;
    json result = json::object();
    for (const std::size_t index : {0, 1, 3, 5, 9, 13}) {
        if (index * 8 >= descriptors.bytes.size())
            continue;
        const auto rva = descriptors.number(index * 8, 4),
                   size = descriptors.number(index * 8 + 4, 4);
        if (!rva && !size)
            continue;
        try {
            if (!rva || !size)
                throw std::runtime_error("Incomplete directory descriptor");
            const auto data = image.at(rva).slice(0, static_cast<std::size_t>(size));
            switch (index) {
            case 0:
                result["exports"] = exports(image, data, rva, budget);
                break;
            case 1:
                result["imports"] = imports(image, data, budget);
                break;
            case 3:
                result["exceptions"] = exceptions(data, machine, budget);
                break;
            case 5:
                result["base_relocations"] = relocations(data, budget);
                break;
            case 9:
                result["tls"] = tls(image, data, base, budget);
                break;
            case 13:
                result["delay_imports"] = delay_imports(image, data, budget);
                break;
            }
        } catch (const std::runtime_error &error) {
            throw std::runtime_error("PE directory " + std::to_string(index) + ": " + error.what());
        }
    }
    return result;
}
} // namespace xs::pe
