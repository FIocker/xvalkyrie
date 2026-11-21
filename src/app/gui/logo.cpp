#include "app/gui/logo.hpp"
#include <glad/gl.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <chrono>
#include <fstream>
#include <iterator>
#include <stb_image.h>
#include <vector>

namespace xv {
Logos::~Logos() {
    for (auto &[path, decoding] : pending_)
        decoding.wait();
    for (const auto &[path, image] : cache_)
        if (image.texture)
            glDeleteTextures(1, &image.texture);
}
void Logos::forget(const std::filesystem::path &path) {
    const auto it = cache_.find(path);
    if (it == cache_.end())
        return;
    if (it->second.texture)
        glDeleteTextures(1, &it->second.texture);
    cache_.erase(it);
}
const LogoImage &Logos::get(const std::filesystem::path &path) {
    static const LogoImage none;
    if (const auto it = cache_.find(path); it != cache_.end())
        return it->second;
    if (const auto it = pending_.find(path); it != pending_.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return none;
        const auto pixels = it->second.get();
        pending_.erase(it);
        LogoImage image;
        if (!pixels.rgba.empty()) {
            image.width = pixels.width;
            image.height = pixels.height;
            glGenTextures(1, &image.texture);
            glBindTexture(GL_TEXTURE_2D, image.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixels.rgba.data());
        }
        return cache_.emplace(path, image).first->second;
    }
    // a few at a time; the rest are asked for again on the next frames
    if (pending_.size() >= 4)
        return none;
    pending_.emplace(
        path, std::async(std::launch::async, [path] {
            Pixels result;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > 8 * 1024 * 1024)
                return result;
            std::ifstream file(path, std::ios::binary);
            const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width,
                                       &height, &channels) ||
                width <= 0 || height <= 0 || width > 2048 || height > 2048)
                return result;
            auto *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                 &width, &height, &channels, 4);
            if (!pixels)
                return result;
            result.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(height) * 4);
            result.width = width;
            result.height = height;
            stbi_image_free(pixels);
            return result;
        }));
    return none;
}
} // namespace xv
