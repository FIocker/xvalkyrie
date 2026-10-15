#include "frontend/logo.hpp"
#include <glad/gl.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <fstream>
#include <iterator>
#include <stb_image.h>
#include <vector>

namespace xs {
Logos::~Logos() {
    for (const auto &[path, image] : cache_)
        if (image.texture)
            glDeleteTextures(1, &image.texture);
}
const LogoImage &Logos::get(const std::filesystem::path &path) {
    if (const auto it = cache_.find(path); it != cache_.end())
        return it->second;
    LogoImage image;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size > 0 && size <= 8 * 1024 * 1024) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
        int channels = 0;
        if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &image.width,
                                  &image.height, &channels) &&
            image.width > 0 && image.height > 0 && image.width <= 2048 && image.height <= 2048) {
            auto *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                 &image.width, &image.height, &channels, 4);
            if (pixels) {
                glGenTextures(1, &image.texture);
                glBindTexture(GL_TEXTURE_2D, image.texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pixels);
                stbi_image_free(pixels);
            }
        }
    }
    return cache_.emplace(path, image).first->second;
}
} // namespace xs
