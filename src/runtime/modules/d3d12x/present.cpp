#include "backends/graphics/backend.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/window.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dxgi1_6.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// How a GDK title shows its frames. The console has no swap chain: the title renders into
// resources of its own and hands one to ID3D12CommandQueue::PresentX, and paces itself with
// the device's frame events. The host shows pictures through a swap chain, so there is one
// here, on the title surface, of the size and format of what the title presents: each
// PresentX copies the title's resource into its back buffer and presents that.
namespace xs::runtime::modules::d3d12x {
namespace {
// D3D12XBOX_PRESENT_PLANE_PARAMETERS
struct PlaneParameters {
    UINT64 token;
    UINT resource_count;
    ID3D12Resource **resources;
    D3D12_RECT *source_rectangles;
    void *placement;
    UINT color_space, scale_filter, extended_count;
    void *extended;
    UINT flags;
};
static_assert(sizeof(PlaneParameters) == 0x48);

struct Presenter {
    IDXGISwapChain3 *chain = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};
std::mutex mutex;
Presenter presenter;
std::atomic<std::uint64_t> presents{0};
// SetFrameIntervalX: the length of a frame, 60 Hz until the title says otherwise
std::atomic<UINT> interval_microseconds{16667};

// a format a flip swap chain takes, for what the title presents; UNKNOWN if there is none
DXGI_FORMAT chain_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
void release(Presenter &state) {
    for (IUnknown *object :
         {static_cast<IUnknown *>(state.chain), static_cast<IUnknown *>(state.list),
          static_cast<IUnknown *>(state.allocator), static_cast<IUnknown *>(state.fence)})
        if (object)
            object->Release();
    state = {};
}
void wait_for(ID3D12Fence *fence, UINT64 value) {
    if (fence->GetCompletedValue() >= value)
        return;
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, 5000);
    CloseHandle(event);
}
// the swap chain for pictures like `desc`, made again when the title changes what it presents
HRESULT prepare(ID3D12CommandQueue *queue, const D3D12_RESOURCE_DESC &desc) {
    const auto format = chain_format(desc.Format);
    const auto width = static_cast<UINT>(desc.Width);
    if (presenter.chain && presenter.width == width && presenter.height == desc.Height &&
        presenter.format == format)
        return S_OK;
    if (presenter.fence)
        wait_for(presenter.fence, presenter.value);
    release(presenter);
    ID3D12Device *device = nullptr;
    IDXGIFactory4 *factory = nullptr;
    IDXGISwapChain1 *chain = nullptr;
    auto result = queue->GetDevice(IID_PPV_ARGS(&device));
    if (SUCCEEDED(result))
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    auto *const window = static_cast<HWND>(title_window().native());
    DXGI_SWAP_CHAIN_DESC1 chain_desc{};
    chain_desc.Width = width;
    chain_desc.Height = desc.Height;
    chain_desc.Format = format;
    chain_desc.SampleDesc = {1, 0};
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = 2;
    chain_desc.Scaling = DXGI_SCALING_STRETCH;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(result))
        result = factory->CreateSwapChainForHwnd(queue, window, &chain_desc, nullptr, nullptr,
                                                 &chain);
    if (SUCCEEDED(result)) {
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        result = chain->QueryInterface(IID_PPV_ARGS(&presenter.chain));
    }
    if (SUCCEEDED(result))
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&presenter.allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presenter.allocator,
                                           nullptr, IID_PPV_ARGS(&presenter.list));
    if (SUCCEEDED(result))
        result = presenter.list->Close();
    if (SUCCEEDED(result))
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenter.fence));
    for (IUnknown *object : {static_cast<IUnknown *>(chain), static_cast<IUnknown *>(factory),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    std::fprintf(stderr,
                 "[Runtime] PresentX: a swap chain %ux%u format %u for the title's format %u on "
                 "the title surface: 0x%08lx\n",
                 width, desc.Height, static_cast<unsigned>(format),
                 static_cast<unsigned>(desc.Format), static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result)) {
        release(presenter);
        return result;
    }
    presenter.width = width;
    presenter.height = desc.Height;
    presenter.format = format;
    return S_OK;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
// XS_D3D_PROBE=<frame>: what the title presents at that frame, and its render targets
void probe(ID3D12Resource *picture) {
    static const int at = [] {
        const auto *const text = std::getenv("XS_D3D_PROBE");
        const int frame = text ? std::atoi(text) : 0;
        return !text ? 0 : frame > 1 ? frame : 300;
    }();
    if (!at || presents.load() + 1 != static_cast<std::uint64_t>(at))
        return;
    std::vector<std::uint8_t> data;
    unsigned pitch = 0, width = 0, height = 0;
    DXGI_FORMAT format{};
    if (read_back(picture, data, pitch, width, height, format))
        report_picture(
            data.data(), pitch, width, height, format,
            ("back buffer (" + std::to_string(draws_into_target(picture)) + " draws)").c_str());
    int index = 0;
    for (auto *const target : bound_render_targets())
        if (target != picture && read_back(target, data, pitch, width, height, format,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET))
            report_picture(data.data(), pitch, width, height, format,
                           ("render target " + std::to_string(index++) + " (" +
                            std::to_string(draws_into_target(target)) + " draws)")
                               .c_str());
    report_failure();
}
} // namespace

HRESULT present_x(ID3D12CommandQueue *queue, UINT planes, const void *plane_parameters,
                  const void *) {
    // no planes: the title shows nothing, which it does while it starts and suspends
    const auto *const plane = static_cast<const PlaneParameters *>(plane_parameters);
    if (!planes || !plane || !plane->resource_count || !plane->resources ||
        !plane->resources[0])
        return S_OK;
    auto *const picture = plane->resources[0];
    const auto desc = picture->GetDesc();
    if (chain_format(desc.Format) == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count > 1) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] PresentX of a resource in format %u with %u samples, which "
                         "no swap chain of the host takes\n",
                         static_cast<unsigned>(desc.Format), desc.SampleDesc.Count);
        return S_OK;
    }
    const std::lock_guard lock(mutex);
    if (const auto result = prepare(queue, desc); FAILED(result))
        return result;
    probe(picture);
    auto &state = presenter;
    wait_for(state.fence, state.value);
    ID3D12Resource *back = nullptr;
    auto result =
        state.chain->GetBuffer(state.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    if (SUCCEEDED(result))
        result = state.allocator->Reset();
    if (SUCCEEDED(result))
        result = state.list->Reset(state.allocator, nullptr);
    if (SUCCEEDED(result)) {
        // the title leaves what it presents in the present state, which is where it stays
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(picture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
        state.list->ResourceBarrier(2, barriers);
        state.list->CopyResource(back, picture);
        std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
        state.list->ResourceBarrier(2, barriers);
        result = state.list->Close();
    }
    if (SUCCEEDED(result)) {
        ID3D12CommandList *const lists[] = {state.list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(state.fence, ++state.value);
        // one display refresh per frame interval of the title's
        const UINT refreshes = std::clamp((interval_microseconds.load() + 8333) / 16667, 1u, 4u);
        result = state.chain->Present(refreshes, 0);
    }
    if (back)
        back->Release();
    graphics::frame_counted();
    ++presents;
    title_window().show();
    // a hidden window does not wait for the display, and a title that sees a status stops
    if (result == DXGI_STATUS_OCCLUDED)
        Sleep(16);
    if (FAILED(result)) {
        if (static std::atomic<bool> reported{false}; !reported.exchange(true)) {
            std::fprintf(stderr, "[Runtime] PresentX failed on the host: 0x%08lx\n",
                         static_cast<unsigned long>(result));
            report_failure();
        }
        return result;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE set_frame_interval(ID3D12Device *, void *, UINT microseconds, UINT,
                                             UINT) {
    if (microseconds)
        interval_microseconds = microseconds;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE schedule_frame_event(ID3D12Device *, UINT, UINT, void *, UINT) {
    return S_OK;
}
// The start of the next frame. A title that presents is held to the display's rate by the
// swap chain; one that waits without presenting in between is held here.
HRESULT STDMETHODCALLTYPE wait_frame_event(ID3D12Device *, UINT, UINT, void *, UINT,
                                           UINT64 *token) {
    using Clock = std::chrono::steady_clock;
    static std::mutex wait_mutex;
    static Clock::time_point last{};
    static std::uint64_t presents_seen = ~std::uint64_t{0};
    static UINT64 next_token = 0;
    const std::lock_guard lock(wait_mutex);
    const auto frame = std::chrono::microseconds(interval_microseconds.load());
    if (presents_seen == presents.load() && Clock::now() < last + frame)
        std::this_thread::sleep_until(last + frame);
    last = Clock::now();
    presents_seen = presents.load();
    if (token)
        *token = ++next_token;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE frame_statistics(ID3D12Device *, UINT64, UINT, UINT *count, void *) {
    if (count)
        *count = 0;
    return S_OK;
}

void report_picture(const std::uint8_t *data, unsigned pitch, unsigned width, unsigned height,
                    DXGI_FORMAT format, const char *what) {
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool ten_bits =
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    // one pixel as blue, green, red, alpha
    const auto pixel = [&](UINT x, UINT y, std::uint8_t *out) {
        const auto *in = data + std::size_t{y} * pitch + std::size_t{x} * 4;
        if (ten_bits) {
            std::uint32_t value;
            std::memcpy(&value, in, 4);
            out[2] = static_cast<std::uint8_t>((value & 0x3ff) >> 2);
            out[1] = static_cast<std::uint8_t>(((value >> 10) & 0x3ff) >> 2);
            out[0] = static_cast<std::uint8_t>(((value >> 20) & 0x3ff) >> 2);
            out[3] = static_cast<std::uint8_t>((value >> 30) * 85);
        } else {
            std::memcpy(out, in, 4);
            if (rgba)
                std::swap(out[0], out[2]);
        }
    };
    double sum[4]{};
    std::uint8_t bgra[4];
    for (UINT y = 0; y < height; y += 8)
        for (UINT x = 0; x < width; x += 8) {
            pixel(x, y, bgra);
            sum[0] += bgra[2], sum[1] += bgra[1], sum[2] += bgra[0], sum[3] += bgra[3];
        }
    if (const auto *folder = std::getenv("TEMP"); folder && format != DXGI_FORMAT_D32_FLOAT)
        if (auto *file =
                std::fopen((std::string(folder) + "\\xs_" + what + ".bmp").c_str(), "wb")) {
            const std::uint32_t pixels = width * height * 4;
            std::uint8_t header[54]{'B', 'M'};
            const std::uint32_t size = 54 + pixels, offset = 54, info = 40,
                                planes_bits = 1 | (32 << 16);
            const std::int32_t signed_width = width,
                               signed_height = -static_cast<std::int32_t>(height);
            std::memcpy(header + 2, &size, 4);
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &info, 4);
            std::memcpy(header + 18, &signed_width, 4);
            std::memcpy(header + 22, &signed_height, 4);
            std::memcpy(header + 26, &planes_bits, 4);
            std::fwrite(header, 1, sizeof header, file);
            std::vector<std::uint8_t> row(std::size_t{width} * 4);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x)
                    pixel(x, y, row.data() + std::size_t{x} * 4);
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
        }
    const double count = (width / 8.0) * (height / 8.0);
    std::fprintf(stderr, "[Runtime] %s %ux%u format %u average %.1f %.1f %.1f %.1f\n", what, width,
                 height, static_cast<unsigned>(format), sum[0] / count, sum[1] / count,
                 sum[2] / count, sum[3] / count);
}
} // namespace xs::runtime::modules::d3d12x
