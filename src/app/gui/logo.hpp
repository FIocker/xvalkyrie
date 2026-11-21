#pragma once
#include <filesystem>
#include <future>
#include <map>
#include <vector>

namespace xv {
struct LogoImage {
    unsigned int texture = 0;
    int width = 0, height = 0;
};
// Pictures of the library as textures, read from their files once each. Files are decoded
// off the window thread: get() is empty for a picture until it is ready.
class Logos {
  public:
    Logos() = default;
    ~Logos();
    Logos(const Logos &) = delete;
    Logos &operator=(const Logos &) = delete;
    const LogoImage &get(const std::filesystem::path &path);
    // lets go of a picture that was read, so that it is read again the next time
    void forget(const std::filesystem::path &path);
    // some picture asked for is still being read
    bool loading() const { return !pending_.empty(); }

  private:
    struct Pixels {
        std::vector<unsigned char> rgba;
        int width = 0, height = 0;
    };
    std::map<std::filesystem::path, LogoImage> cache_;
    std::map<std::filesystem::path, std::future<Pixels>> pending_;
};
} // namespace xv
