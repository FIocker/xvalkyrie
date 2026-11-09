#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace xs::host {
// folder of the running executable
std::filesystem::path executable_directory();
// Starts this executable again without arguments and ends this process at once, whatever
// its threads are doing. Returns only if the new process could not be started.
void restart_process();
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
