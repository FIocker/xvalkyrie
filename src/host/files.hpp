#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace xs::host {
// folder of the running executable
std::filesystem::path executable_directory();
// Runs this executable again with `arguments` and waits for it: its exit code. `process`
// holds the child while it runs, for end_process from another thread, and null otherwise.
std::uint32_t run_self(const std::vector<std::wstring> &arguments, std::atomic<void *> &process);
// Ends a process run_self started, whatever it is doing
void end_process(void *process);
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
} // namespace xs::host
