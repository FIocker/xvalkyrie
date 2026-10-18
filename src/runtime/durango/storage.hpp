#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xs::runtime::durango {
// One connected storage space (the machine's, or the user's) kept in host files:
// `directory`/<container>/<blob>, every name escaped so that any text the title picks is a
// valid file name on every host. Names are UTF-8. Nothing is kept in memory: every call
// goes to the files, so what a title wrote survives the process.
class SaveSpace {
  public:
    explicit SaveSpace(std::filesystem::path directory);
    struct Container {
        std::string name;
        std::string display_name; // empty unless the title gave one
        std::uint64_t size = 0;   // of all its blobs
        std::chrono::system_clock::time_point modified;
    };
    struct Blob {
        std::string name;
        std::uint64_t size = 0;
    };
    // Writes and deletes blobs of a container, creating it if needed. Every new blob is
    // written in full before any of them replaces the old one, so a failure leaves the
    // container as it was. Throws std::runtime_error on failure.
    void submit(const std::string &container,
                const std::map<std::string, std::vector<std::uint8_t>> &writes,
                const std::vector<std::string> &deletes,
                const std::optional<std::string> &display_name = {});
    // nullopt if the container or the blob does not exist
    std::optional<std::vector<std::uint8_t>> read(const std::string &container,
                                                  const std::string &blob) const;
    // false if there was no such container
    bool remove(const std::string &container);
    // the containers whose name starts with `prefix`, by name
    std::vector<Container> containers(const std::string &prefix = {}) const;
    // the blobs of a container whose name starts with `prefix`, by name
    std::vector<Blob> blobs(const std::string &container, const std::string &prefix = {}) const;

  private:
    std::filesystem::path directory_;
};
// The text in a file name of a SaveSpace, and back. Exposed for the tests.
std::string escape_save_name(const std::string &name);
std::optional<std::string> unescape_save_name(const std::string &file);
} // namespace xs::runtime::durango
