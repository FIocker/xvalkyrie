#pragma once
#include "core/target.hpp"
#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace xv {
enum class LaunchStatus {
    unreadable,
    invalid_image,
    wrong_machine,
    package_unsupported, // XVD/XVC container; identified but never opened
    other_target,        // imports indicate a Windows PC build or a mix of targets
    runtime_unavailable,
    runnable // console target known, every import bound, host can execute; nothing ran yet
};
struct LaunchResult {
    std::string file;
    LaunchStatus status;
    std::string detail;
    std::optional<Target> target = {}; // console the imports point to, if any
};
// `path` is an executable, an unpacked title directory, or the MicrosoftGame.config or
// AppxManifest.xml of one.
LaunchResult launch_title(const std::filesystem::path &path);
// What the title `path` stands for says about itself (name, display_name, version, title_id,
// logo, splash, applications), from its MicrosoftGame.config or its AppxManifest.xml, looked
// up in the directory and in its Mount subdirectory. `directory` is set to where it was
// found. Null if `path` is a file that is neither. A display name the manifest only refers
// to ("ms-resource:AppName") is read from the package's resources.pri; title_id is the Xbox
// Live title ID, eight hexadecimal digits in capitals, null without one.
nlohmann::json title_manifest(const std::filesystem::path &path, std::filesystem::path &directory);
// The executable `path` stands for: the first application of the manifest for a title
// directory (looked up in it and in its Mount subdirectory), `path` itself otherwise.
std::filesystem::path title_executable(const std::filesystem::path &path);
// What to call the title `path` stands for: the display name of its manifest, or of the
// package an executable is in; the name of the folder or file where there is none. UTF-8.
std::string title_name(const std::filesystem::path &path);
// Everything that can be told about the title `path` stands for without running it, for
// someone looking into why it does or does not work:
//   path, executable, save_directory, system_directory, target
//   manifest       what title_manifest gives, with manifest_directory
//   image          the PE headers: machine, image_base, size_of_image, entry_rva...
//   mapping        where the loader puts it: guest_base, relocation, sections
//   modules        each imported module: category, and how many of its imports bind to
//                  XValkyrie (own), the host, the title's DLLs, the console's system
//                  modules, or nothing (missing)
//   imports        each import: module, symbol, origin
//   errors         what could not be worked out, by the name of the part
// Reads and maps the executable, so it takes a while. Never throws and never runs it.
nlohmann::json title_properties(const std::filesystem::path &path);
// What an exit code of a title's process says, in a few words: the runtime's own codes,
// the host's name for an exception or a status, or that the code is the title's own.
std::string exit_description(std::uint32_t code);
// The Xbox Live title ID of the title `path` stands for ("799B973E"), empty if its manifest
// has none or it has no manifest. Never throws.
std::string title_id(const std::filesystem::path &path);
// Where the saved games of the title `path` stands for are kept: saves/<package name> in the
// configuration folder, or saves/<executable name> for a title without a manifest.
std::filesystem::path title_save_directory(const std::filesystem::path &path);
} // namespace xv
