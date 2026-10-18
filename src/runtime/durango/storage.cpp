#include "runtime/durango/storage.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace xs::runtime::durango {
namespace fs = std::filesystem;
namespace {
// Files of a container that are not blobs start with a character escaping never produces.
constexpr char display_file[] = "#display";
constexpr char new_prefix[] = "#new-";

fs::path file_path(const std::string &name) {
    return fs::path(std::u8string(name.begin(), name.end()));
}
std::string file_text(const fs::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
bool reserved(std::string_view name) {
    // device names Windows will not create a file for, whatever the extension
    const auto stem = name.substr(0, name.find('.'));
    for (const std::string_view device : {"con", "prn", "aux", "nul"})
        if (stem == device)
            return true;
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem[3] >= '0' && stem[3] <= '9';
}
std::vector<std::uint8_t> load(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + file_text(path));
    return {std::istreambuf_iterator<char>(file), {}};
}
void store(const fs::path &path, const void *data, std::size_t size) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write " + file_text(path));
}
std::chrono::system_clock::time_point system_time(fs::file_time_type time) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
}
void check_name(const std::string &name, const char *what) {
    if (name.empty())
        throw std::runtime_error(std::string("A connected storage ") + what + " needs a name");
}
} // namespace

// Letters, digits, '_' and '-' stay as they are, an upper case letter becomes '^' and the
// letter in lower case (Windows does not tell file names apart by case), '.' stays except
// at either end, and every other byte is %XX.
std::string escape_save_name(const std::string &name) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string lowered;
    for (const char c : name)
        lowered += c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    const bool device = reserved(lowered);
    std::string result;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        const bool edge = i == 0 || i + 1 == name.size();
        if (c >= 'A' && c <= 'Z' && !(device && i == 0))
            result += {'^', static_cast<char>(c + 32)};
        else if (((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  (c == '.' && !edge)) &&
                 !(device && i == 0))
            result += static_cast<char>(c);
        else
            result += {'%', digits[c >> 4], digits[c & 15]};
    }
    return result;
}
std::optional<std::string> unescape_save_name(const std::string &file) {
    const auto hex = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string result;
    for (std::size_t i = 0; i < file.size(); ++i) {
        const char c = file[i];
        if (c == '^') {
            if (i + 1 >= file.size() || file[i + 1] < 'a' || file[i + 1] > 'z')
                return {};
            result += static_cast<char>(file[++i] - 32);
        } else if (c == '%') {
            if (i + 2 >= file.size() || hex(file[i + 1]) < 0 || hex(file[i + 2]) < 0)
                return {};
            result += static_cast<char>(hex(file[i + 1]) << 4 | hex(file[i + 2]));
            i += 2;
        } else if (c == '#') {
            return {}; // ours, not a blob
        } else {
            result += c;
        }
    }
    if (result.empty())
        return {};
    return result;
}

SaveSpace::SaveSpace(fs::path directory) : directory_(std::move(directory)) {}

void SaveSpace::submit(const std::string &container,
                       const std::map<std::string, std::vector<std::uint8_t>> &writes,
                       const std::vector<std::string> &deletes,
                       const std::optional<std::string> &display_name) {
    check_name(container, "container");
    const auto folder = directory_ / file_path(escape_save_name(container));
    fs::create_directories(folder);
    std::vector<std::pair<fs::path, fs::path>> staged; // new file, final file
    try {
        for (const auto &[name, bytes] : writes) {
            check_name(name, "blob");
            const auto escaped = escape_save_name(name);
            staged.emplace_back(folder / file_path(new_prefix + escaped),
                                folder / file_path(escaped));
            store(staged.back().first, bytes.data(), bytes.size());
        }
        if (display_name) {
            staged.emplace_back(folder / (std::string(new_prefix) + (display_file + 1)),
                                folder / display_file);
            store(staged.back().first, display_name->data(), display_name->size());
        }
    } catch (...) {
        std::error_code ignored;
        for (const auto &[temporary, final] : staged)
            fs::remove(temporary, ignored);
        throw;
    }
    for (const auto &[temporary, final] : staged)
        fs::rename(temporary, final);
    for (const auto &name : deletes) {
        if (name.empty() || writes.contains(name))
            continue;
        std::error_code error;
        fs::remove(folder / file_path(escape_save_name(name)), error);
        if (error)
            throw std::runtime_error("Cannot delete blob " + name + ": " + error.message());
    }
}

std::optional<std::vector<std::uint8_t>> SaveSpace::read(const std::string &container,
                                                         const std::string &blob) const {
    if (container.empty() || blob.empty())
        return {};
    const auto path =
        directory_ / file_path(escape_save_name(container)) / file_path(escape_save_name(blob));
    std::error_code error;
    if (!fs::is_regular_file(path, error))
        return {};
    return load(path);
}

bool SaveSpace::remove(const std::string &container) {
    if (container.empty())
        return false;
    std::error_code error;
    const auto removed = fs::remove_all(directory_ / file_path(escape_save_name(container)), error);
    if (error)
        throw std::runtime_error("Cannot delete container " + container + ": " + error.message());
    return removed > 0;
}

std::vector<SaveSpace::Container> SaveSpace::containers(const std::string &prefix) const {
    std::vector<Container> result;
    std::error_code error;
    for (fs::directory_iterator it(directory_, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_directory(error))
            continue;
        const auto name = unescape_save_name(file_text(it->path().filename()));
        if (!name || !name->starts_with(prefix))
            continue;
        Container container{*name, {}, 0, system_time(it->last_write_time(error))};
        for (const auto &blob : blobs(*name)) {
            container.size += blob.size;
        }
        std::error_code ignored;
        for (const auto &entry : fs::directory_iterator(it->path(), ignored))
            if (entry.is_regular_file(ignored))
                container.modified =
                    std::max(container.modified, system_time(entry.last_write_time(ignored)));
        if (fs::is_regular_file(it->path() / display_file, ignored)) {
            const auto bytes = load(it->path() / display_file);
            container.display_name.assign(bytes.begin(), bytes.end());
        }
        result.push_back(std::move(container));
    }
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return result;
}

std::vector<SaveSpace::Blob> SaveSpace::blobs(const std::string &container,
                                              const std::string &prefix) const {
    std::vector<Blob> result;
    if (container.empty())
        return result;
    std::error_code error;
    for (fs::directory_iterator it(directory_ / file_path(escape_save_name(container)), error), end;
         !error && it != end; it.increment(error)) {
        std::error_code ignored;
        if (!it->is_regular_file(ignored))
            continue;
        const auto name = unescape_save_name(file_text(it->path().filename()));
        if (name && name->starts_with(prefix))
            result.push_back({*name, it->file_size(ignored)});
    }
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return result;
}
} // namespace xs::runtime::durango
