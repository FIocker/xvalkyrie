#include "backends/graphics/backend.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <wrl/client.h>

// XS_FRAME_DUMP=<file.bmp> writes the back buffer of one presented frame (number
// XS_FRAME_DUMP_AT, 60 if unset) to a file: a way to see what a title draws without
// looking at the window.
namespace xs::graphics::d3d11 {
namespace {
using Microsoft::WRL::ComPtr;

bool write_bitmap(const char *path, IDXGISwapChain *swap_chain) {
    ComPtr<ID3D11Texture2D> back_buffer, copy;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return false;
    back_buffer->GetDevice(&device);
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC description;
    back_buffer->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &copy)))
        return false;
    context->CopyResource(copy.Get(), back_buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto width = description.Width, height = description.Height;
    std::vector<unsigned char> pixels(std::size_t{width} * height * 3);
    for (UINT y = 0; y < height; ++y) {
        const auto *row = static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch;
        auto *out = pixels.data() + std::size_t{height - 1 - y} * width * 3; // bottom-up
        for (UINT x = 0; x < width; ++x, row += 4, out += 3) {
            out[0] = row[bgra ? 0 : 2];
            out[1] = row[1];
            out[2] = row[bgra ? 2 : 0];
        }
    }
    context->Unmap(copy.Get(), 0);
    const auto size = static_cast<std::uint32_t>(pixels.size());
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte)
            header[at + byte] = static_cast<unsigned char>(value >> (8 * byte));
    };
    put(2, 54 + size);
    put(10, 54);
    put(14, 40);
    put(18, width);
    put(22, height);
    header[26] = 1;
    header[28] = 24;
    put(34, size);
    auto *const file = std::fopen(path, "wb");
    if (!file)
        return false;
    const bool written = std::fwrite(header, 1, sizeof header, file) == sizeof header &&
                         std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    return std::fclose(file) == 0 && written;
}
} // namespace

void frame_presented(IDXGISwapChain *swap_chain) {
    static const char *const path = std::getenv("XS_FRAME_DUMP");
    static std::atomic<long> frames{0};
    const auto frame = ++frames;
    frame_counted();
    // and every 10000 after, so that a long run shows when frames stop
    if (frame == 1 || frame == 10 || frame == 100 || frame == 1000 || frame % 10000 == 0) {
        std::fprintf(stderr, "[Runtime] %ld frame%s presented\n", frame, frame == 1 ? "" : "s");
        std::fflush(stderr);
    }
    if (!path)
        return;
    static const long wanted = [] {
        const auto *const text = std::getenv("XS_FRAME_DUMP_AT");
        return text ? std::atol(text) : 60;
    }();
    if (frame != wanted)
        return;
    const bool written = write_bitmap(path, swap_chain);
    std::fprintf(stderr, "[Runtime] frame %ld %s %s\n", frame,
                 written ? "written to" : "could not be written to", path);
    std::fflush(stderr);
}
} // namespace xs::graphics::d3d11
