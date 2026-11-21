#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace xv::pe {
struct Bytes {
    std::span<const std::uint8_t> bytes;

    Bytes slice(std::size_t pos, std::size_t length) const {
        if (pos > bytes.size() || length > bytes.size() - pos)
            throw std::runtime_error("PE range extends beyond its containing region");
        return {bytes.subspan(pos, length)};
    }
    std::uint64_t number(std::size_t pos, std::size_t width) const {
        std::uint64_t value = 0;
        const auto field = slice(pos, width);
        for (std::size_t i = 0; i < width; ++i)
            value |= std::uint64_t{field.bytes[i]} << (8 * i);
        return value;
    }
};

struct Image {
    struct Region {
        std::uint64_t start, end;
        Bytes raw;
    };
    std::vector<Region> regions;

    void add(std::uint64_t start, std::uint64_t extent, Bytes raw) {
        if (!extent)
            return;
        const auto end = start + extent;
        if (end > (std::uint64_t{1} << 32))
            throw std::runtime_error("Section exceeds PE RVA space");
        for (const auto &region : regions)
            if (start < region.end && region.start < end)
                throw std::runtime_error("Overlapping PE RVA regions");
        regions.push_back({start, end, raw});
    }
    // Return only consecutive on-disk bytes, never a zero-filled virtual tail.
    Bytes at(std::uint64_t rva) const {
        for (const auto &region : regions) {
            if (rva >= region.start && rva - region.start < region.raw.bytes.size())
                return {region.raw.bytes.subspan(static_cast<std::size_t>(rva - region.start))};
        }
        throw std::runtime_error("RVA has no file-backed bytes");
    }
};
} // namespace xv::pe
