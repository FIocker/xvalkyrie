#pragma once
#include "host/memory.hpp"
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xv::loader {
// Unresolved import slots get this tag + their index. It is a non-canonical
// address (also with 5-level paging), so calling it always faults.
inline constexpr std::uint64_t unresolved_tag = 0x4000'0000'0000'0000;
inline constexpr std::uint64_t image_limit = std::uint64_t{1} << 30;

struct MapOptions {
    // Base address the image is relocated to. Empty = wherever the host mapped it,
    // which is what you want for running it. Must be 64K aligned.
    std::optional<std::uint64_t> guest_base;
    // (module, "Name" or "#ordinal") -> address
    std::function<std::optional<std::uint64_t>(std::string_view, std::string_view)> resolve;
    // Make code sections executable. Needs guest_base to be empty. W+X sections are rejected.
    bool executable = false;
};

// "Name" or "#ordinal" for an import entry of the PE report
std::string import_label(const nlohmann::json &symbol);

// PE32+ image mapped in memory: sections copied, relocations applied, imports bound.
// Does not run anything, set up TLS or load dependencies.
class MappedImage {
  public:
    explicit MappedImage(std::span<const std::uint8_t> file, const MapOptions &options = {});
    // the same, with the pe::inspect report of `file` already at hand (light is enough)
    MappedImage(std::span<const std::uint8_t> file, const nlohmann::json &pe,
                const MapOptions &options = {});
    std::uint64_t guest_base() const { return base_; }
    // throws if the RVA is not inside the headers or a section
    std::uint64_t read64(std::uint64_t rva) const;
    const nlohmann::json &report() const { return report_; }

  private:
    std::uint64_t size_;
    host::Reservation memory_;
    std::uint64_t base_ = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> readable_;
    nlohmann::json report_;
};
} // namespace xv::loader
