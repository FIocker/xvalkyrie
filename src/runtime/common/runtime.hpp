#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace xs::runtime {
// exit codes produced by the runtime, not by the guest
inline constexpr std::uint32_t exit_unimplemented_import = 0xE0585301;
inline constexpr std::uint32_t exit_guest_fault = 0xE0585302;

// Our implementations of guest imports. Module names are case-insensitive,
// symbols are "Name" or "#ordinal". Functions must use the MS x64 ABI.
class Registry {
  public:
    void add(std::string_view module, std::string_view symbol, void *function);
    void *find(std::string_view module, std::string_view symbol) const;

  private:
    std::map<std::pair<std::string, std::string>, void *> entries_;
};

// only true on Windows x64
bool execution_supported();
// Where each import would end up (own / host / missing). Does not run anything.
nlohmann::json bind_plan(std::span<const std::uint8_t> file, const Registry &registry);
// Maps and runs the image, returns its exit code. No sandbox!
// If allow_missing is false it throws before starting when an import can't be resolved.
std::uint32_t execute(const std::filesystem::path &path, std::span<const std::uint8_t> file,
                      const Registry &registry, bool allow_missing);

namespace detail {
// per-host hooks, null = nothing available
void *runtime_override(std::string_view module, std::string_view symbol);
void *host_export(std::string_view module, std::string_view symbol);
bool host_policy_allows(std::string_view module, std::string_view symbol);
} // namespace detail
} // namespace xs::runtime
