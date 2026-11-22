#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xv::host {
// folder of the running executable
std::filesystem::path executable_directory();
// Runs the executable `name` of this one's folder with `arguments` and waits for it: its
// exit code. `process` holds the child while it runs, for end_process from another thread,
// and null otherwise. The child ends with this process, however this one goes. With
// `output`, what the child writes to stdout and stderr comes there instead, as it is
// written, in pieces that need not end where a line does, on the calling thread; without
// it the child writes to this one's console if it has one.
std::uint32_t run_beside(const std::wstring &name, const std::vector<std::wstring> &arguments,
                         std::atomic<void *> &process,
                         const std::function<void(std::string_view)> &output = {});
// Ends a process run_beside started, whatever it is doing
void end_process(void *process);
// Ends this process at once with `code`, whatever its threads are doing and without
// running anything else in it.
[[noreturn]] void end_now(std::uint32_t code);
// The host's words for an exit code that is one of its exceptions or statuses ("Access
// violation", "Stack overflow"...), UTF-8; empty for a code it has no words for.
std::string status_text(std::uint32_t code);
// Shows a folder, or the folder a file is in with the file selected, in the host's file
// manager. False if it could not be asked to.
bool show_in_file_manager(const std::filesystem::path &path);
// Puts a shortcut called `name` on the user's desktop that runs the executable `program`
// of this one's folder with `arguments`, its console minimized. The file made, empty if it
// could not be.
std::filesystem::path create_desktop_shortcut(const std::wstring &name, const std::wstring &program,
                                              const std::wstring &arguments);
// Opens a file with whatever the host opens its kind with. False if it could not.
bool open_with_host(const std::filesystem::path &path);
std::vector<std::uint8_t> read_snapshot(const std::filesystem::path &path, std::size_t limit);

// first `count` bytes of a file (or fewer), plus its total size
struct Prefix {
    std::vector<std::uint8_t> bytes;
    std::uint64_t file_size;
};
Prefix read_prefix(const std::filesystem::path &path, std::size_t count);

// For stable regular files only.
class FileMapping {
  public:
    FileMapping(const std::filesystem::path &path, std::size_t limit);
    ~FileMapping();
    FileMapping(const FileMapping &) = delete;
    FileMapping &operator=(const FileMapping &) = delete;
    std::span<const std::uint8_t> bytes() const { return bytes_; }

  private:
    std::span<const std::uint8_t> bytes_;
};
} // namespace xv::host
