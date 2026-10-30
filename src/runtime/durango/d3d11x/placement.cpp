#include "runtime/durango/d3d11x/placement.hpp"
#include "runtime/modules/xg.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <windows.h>

namespace xs::runtime::durango::d3d11x {
namespace {
struct Placement {
    ID3D11Resource *resource; // one reference of ours
    std::uintptr_t start, end;
    bool dirty;
};
std::mutex mutex;
std::vector<Placement> placements;
PVOID handler = nullptr;

constexpr std::uintptr_t page = 4096;
std::uintptr_t page_down(std::uintptr_t address) { return address & ~(page - 1); }
std::uintptr_t page_up(std::uintptr_t address) { return (address + page - 1) & ~(page - 1); }
void protect(const Placement &placement, DWORD protection) {
    DWORD previous;
    VirtualProtect(reinterpret_cast<void *>(page_down(placement.start)),
                   page_up(placement.end) - page_down(placement.start), protection, &previous);
}

// the first write to a placement's memory since its last upload
LONG CALLBACK written(EXCEPTION_POINTERS *exception) {
    const auto *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    const std::lock_guard lock(mutex);
    bool ours = false;
    for (auto &placement : placements)
        if (address >= page_down(placement.start) && address < page_up(placement.end)) {
            placement.dirty = ours = true;
            protect(placement, PAGE_READWRITE);
        }
    return ours ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

UINT row_bytes(UINT width, DXGI_FORMAT format) {
    const auto size = modules::format_size(static_cast<std::uint32_t>(format));
    width = std::max(width, 1u);
    return size.blocks ? (width + 3) / 4 * size.bytes : width * size.bytes;
}
UINT rows(UINT height, DXGI_FORMAT format) {
    height = std::max(height, 1u);
    return modules::format_size(static_cast<std::uint32_t>(format)).blocks ? (height + 3) / 4
                                                                           : height;
}
// the subresources of a 2D texture in XG's linear layout: per array slice, mips in order
std::size_t texture_layout(const D3D11_TEXTURE2D_DESC &desc, std::uintptr_t address,
                           D3D11_SUBRESOURCE_DATA *data, UINT count) {
    const UINT mips = std::max(desc.MipLevels, 1u);
    std::size_t offset = 0;
    for (UINT slice = 0; slice < std::max(desc.ArraySize, 1u); ++slice)
        for (UINT mip = 0; mip < mips; ++mip) {
            const auto pitch = row_bytes(desc.Width >> mip, desc.Format);
            const auto bytes = std::size_t{pitch} * rows(desc.Height >> mip, desc.Format);
            if (const auto index = slice * mips + mip; data && index < count)
                data[index] = {reinterpret_cast<const void *>(address + offset), pitch,
                               static_cast<UINT>(bytes)};
            offset += bytes;
        }
    return offset;
}
std::size_t resource_bytes(ID3D11Resource *resource) {
    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
        return desc.ByteWidth;
    }
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        return texture_layout(desc, 0, nullptr, 0);
    }
    return 0;
}
bool readable(std::uintptr_t start, std::size_t bytes) {
    for (auto at = page_down(start); at < start + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
void upload(ID3D11DeviceContext *context, const Placement &placement) {
    auto *const resource = placement.resource;
    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
        if (desc.Usage == D3D11_USAGE_DYNAMIC) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context->Map(resource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, reinterpret_cast<const void *>(placement.start),
                            desc.ByteWidth);
                context->Unmap(resource, 0);
            }
        } else if (desc.Usage == D3D11_USAGE_DEFAULT) {
            context->UpdateSubresource(resource, 0, nullptr,
                                       reinterpret_cast<const void *>(placement.start), 0, 0);
        }
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
    if (desc.Usage != D3D11_USAGE_DEFAULT || desc.SampleDesc.Count > 1)
        return;
    std::vector<D3D11_SUBRESOURCE_DATA> data(std::size_t{std::max(desc.MipLevels, 1u)} *
                                             std::max(desc.ArraySize, 1u));
    texture_layout(desc, placement.start, data.data(), static_cast<UINT>(data.size()));
    for (UINT index = 0; index < data.size(); ++index)
        context->UpdateSubresource(resource, index, nullptr, data[index].pSysMem,
                                   data[index].SysMemPitch, data[index].SysMemSlicePitch);
}
} // namespace

void track_placement(ID3D11Resource *resource, const void *address) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto bytes = resource ? resource_bytes(resource) : 0;
    if (!address || !bytes || !readable(start, bytes))
        return;
    const std::lock_guard lock(mutex);
    if (!handler)
        handler = AddVectoredExceptionHandler(1, &written);
    // a resource placed where an older one was replaces it
    std::erase_if(placements, [&](const Placement &old) {
        const bool replaced = old.start == start;
        if (replaced)
            old.resource->Release();
        return replaced;
    });
    resource->AddRef();
    placements.push_back({resource, start, start + bytes, false});
    protect(placements.back(), PAGE_READONLY);
}

bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return address && desc.SampleDesc.Count <= 1 &&
           readable(start, texture_layout(desc, start, data, count));
}

void upload_placements(ID3D11DeviceContext *context) {
    const std::lock_guard lock(mutex);
    for (auto &placement : placements)
        if (placement.dirty) {
            upload(context, placement);
            placement.dirty = false;
            protect(placement, PAGE_READONLY);
        }
}
} // namespace xs::runtime::durango::d3d11x
