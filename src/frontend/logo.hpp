#pragma once
#include <filesystem>
#include <map>

namespace xs {
struct LogoImage {
    unsigned int texture = 0;
    int width = 0, height = 0;
};
class Logos {
  public:
    Logos() = default;
    ~Logos();
    Logos(const Logos &) = delete;
    Logos &operator=(const Logos &) = delete;
    const LogoImage &get(const std::filesystem::path &path);

  private:
    std::map<std::filesystem::path, LogoImage> cache_;
};
} // namespace xs
