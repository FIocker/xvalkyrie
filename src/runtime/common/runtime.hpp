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
    // Lets every import of `module` that has no entry here go to the export of
    // `host_module` with the same name or ordinal, whatever the host policy says.
    void forward(std::string_view module, std::string_view host_module);
    // lowercase host module `module` is forwarded to, empty if it is not
    std::string_view forwarded(std::string_view module) const;

  private:
    std::map<std::pair<std::string, std::string>, void *> entries_;
    std::map<std::string, std::string> forwards_;
};

// only true on Windows x64
// Where each import would end up (own / host / title / missing). Does not run anything.
// "title" is a DLL in `directory`, the folder of the executable, that exports the symbol;
// without a directory there are no such DLLs.
nlohmann::json bind_plan(std::span<const std::uint8_t> file, const Registry &registry,
                         const std::filesystem::path &directory = {});
struct ExecuteOptions {
    // start even if some imports can't be resolved; calling one of them ends the process
    bool allow_missing = false;
    // report the first call of every imported function on stderr, and the last calls
    // before a fault
    bool trace = false;
    // no sound from the title
    bool mute = false;
    // host folder where the title's saved games go, see save_directory()
    std::filesystem::path save_directory{};
};
// Maps and runs the image, returns its exit code. No sandbox!
// A guest that faults ends the process with exit_guest_fault.
std::uint32_t execute(const std::filesystem::path &path, std::span<const std::uint8_t> file,
                      const Registry &registry, const ExecuteOptions &options = {});

// The package a title's executable belongs to: the nearest folder at or above `directory`
// with an AppxManifest.xml, `directory` itself if there is none. A title's own DLLs are
// looked for from the executable's folder up to that one.
std::filesystem::path package_directory(const std::filesystem::path &directory);

// The folder of the running title's saved games, from ExecuteOptions; empty before execute
// or when none was given.
const std::filesystem::path &save_directory();

namespace detail {
void set_save_directory(std::filesystem::path directory);
struct Binding {
    void *function;     // null if missing
    const char *origin; // "own", "host" or "missing"
};
// The one place that decides where an import goes.
Binding resolve(const Registry &registry, std::string_view module, std::string_view symbol);
// replacement for a host function that cannot be used as is inside a guest
void *runtime_override(std::string_view symbol);
// export of a system DLL, by "Name" or "#ordinal"
void *host_export(std::string_view module, std::string_view symbol);
// Whether the import is one the title's own DLL of that name provides: a DLL in
// `directory` that is neither ours nor forwarded and exports the symbol. Never loads it.
// The title's own DLL `module` for an executable in `directory`, empty if it has none
std::filesystem::path title_dll(const std::filesystem::path &directory, std::string_view module);
bool title_provides(const Registry &registry, const std::filesystem::path &directory,
                    std::string_view module, std::string_view symbol);
// offset of an address inside the running guest image, 0 if it is not in it
std::uint64_t guest_offset(const void *address);
// whether the address is inside the running guest image or one of the title's DLLs
bool in_title(const void *address);
// "image+0x...", "<title dll>+0x..." or the plain address, for diagnostics
std::string guest_location(const void *address);
// Export of the title's own DLL `module` while it runs, mapping the DLL and calling its
// DllMain on first use. Null if the title has no such DLL or the DLL does not export it.
void *title_export(std::string_view module, std::string_view symbol);
bool host_policy_allows(std::string_view module, std::string_view symbol);
} // namespace detail
} // namespace xs::runtime
