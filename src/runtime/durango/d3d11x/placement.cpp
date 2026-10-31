#include "runtime/durango/d3d11x/placement.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/xg.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <windows.h>

namespace xs::runtime::durango::d3d11x {
namespace {
struct Placement {
    ID3D11Resource *resource; // one reference of ours
    std::uintptr_t start, end;
    bool dirty;
    UINT tile_mode, pitch;
};
std::mutex mutex;
std::map<std::uintptr_t, Placement> placements; // by start
std::vector<std::uintptr_t> dirty;              // starts of the ones written since uploaded
std::uintptr_t largest = 0;
PVOID handler = nullptr;

constexpr std::uintptr_t page = 4096;
std::uintptr_t page_down(std::uintptr_t address) { return address & ~(page - 1); }
std::uintptr_t page_up(std::uintptr_t address) { return (address + page - 1) & ~(page - 1); }
// region by region: VirtualProtect fails for a range over more than one allocation, and the
// title's physical memory is mapped as many views
void protect(std::uintptr_t start, std::uintptr_t end, DWORD protection) {
    for (auto at = page_down(start); at < page_up(end);) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(at), &info, sizeof info))
            return;
        const auto region_end = std::min(
            page_up(end), reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        DWORD previous;
        if (info.State == MEM_COMMIT)
            VirtualProtect(reinterpret_cast<void *>(at), region_end - at, protection, &previous);
        at = region_end;
    }
}
void protect(const Placement &placement, DWORD protection) {
    protect(placement.start, placement.end, protection);
}
bool writable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
           (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
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
    // the ones whose pages hold the address: they start at most `largest` before it
    for (auto at = placements.upper_bound(page_up(address + 1)); at != placements.begin();) {
        auto &placement = (--at)->second;
        if (page_down(placement.start) + largest + page < address)
            break;
        if (address >= page_down(placement.start) && address < page_up(placement.end)) {
            ours = true;
            if (!placement.dirty)
                dirty.push_back(placement.start);
            placement.dirty = true;
            protect(placement, PAGE_READWRITE);
        }
    }
    // going on is only right if the write can now happen; otherwise it faults again forever
    if (ours && !writable(address))
        protect(address, address + 1, PAGE_READWRITE);
    return ours && writable(address) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
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
// XG_TEXTURE2D_DESC and the two XGTextureAddressComputer vtables of the console's xg_x.dll:
// CopyFromSubresource is at 6 in the older one, at 9 in the newer, which has the resource
// size at 3 where the older has the offset of a mip (0 for the first)
struct XgTexture2D {
    UINT width, height, mips, array_size, format, sample_count, sample_quality, usage, bind_flags,
        cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
using CreateComputer = HRESULT(WINAPI *)(const XgTexture2D *, void **);
using Copy = HRESULT(WINAPI *)(void *, void *, UINT, UINT, const void *, UINT, UINT);
CreateComputer console_xg() {
    static const auto create = reinterpret_cast<CreateComputer>(
        detail::system_export("xg_x.dll", "XGCreateTexture2DComputer"));
    return create;
}
// SEH only, no objects to unwind here
bool copy_out(Copy copy, void *computer, void *destination, UINT subresource, const void *source,
              UINT row_pitch, UINT slice_pitch) {
    __try {
        return SUCCEEDED(
            copy(computer, destination, 0, subresource, source, row_pitch, slice_pitch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool detile(ID3D11DeviceContext *context, ID3D11Texture2D *texture,
            const D3D11_TEXTURE2D_DESC &desc, const Placement &placement) {
    const auto create = console_xg();
    if (!create)
        return false;
    const XgTexture2D xg{desc.Width,
                         desc.Height,
                         desc.MipLevels,
                         desc.ArraySize,
                         static_cast<UINT>(desc.Format),
                         desc.SampleDesc.Count,
                         desc.SampleDesc.Quality,
                         static_cast<UINT>(desc.Usage),
                         desc.BindFlags,
                         desc.CPUAccessFlags,
                         desc.MiscFlags,
                         0,
                         0,
                         placement.tile_mode,
                         placement.pitch};
    void *computer = nullptr;
    if (FAILED(create(&xg, &computer)) || !computer)
        return false;
    auto *const vtable = *static_cast<void ***>(computer);
    const auto first = reinterpret_cast<std::uint64_t(WINAPI *)(void *, UINT, UINT)>(vtable[3]);
    const auto copy = reinterpret_cast<Copy>(vtable[first(computer, 0, 0) ? 9 : 6]);
    const UINT mips = std::max(desc.MipLevels, 1u);
    std::vector<std::byte> linear;
    bool ok = true;
    for (UINT slice = 0; slice < std::max(desc.ArraySize, 1u) && ok; ++slice)
        for (UINT mip = 0; mip < mips && ok; ++mip) {
            const auto pitch = row_bytes(desc.Width >> mip, desc.Format);
            const auto bytes = pitch * rows(desc.Height >> mip, desc.Format);
            linear.resize(bytes);
            const auto index = slice * mips + mip;
            ok = copy_out(copy, computer, linear.data(), index,
                          reinterpret_cast<const void *>(placement.start), pitch, bytes);
            if (ok)
                context->UpdateSubresource(texture, index, nullptr, linear.data(), pitch, bytes);
        }
    reinterpret_cast<ULONG(WINAPI *)(void *)>(vtable[1])(computer);
    return ok;
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
    if (placement.tile_mode > 1) {
        static std::atomic<bool> reported{false};
        if (!detile(context, static_cast<ID3D11Texture2D *>(resource), desc, placement) &&
            !reported.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] a texture in tile mode %u, and no xg_x.dll of the console "
                         "to read it: it stays as it is\n",
                         placement.tile_mode);
        return;
    }
    std::vector<D3D11_SUBRESOURCE_DATA> data(std::size_t{std::max(desc.MipLevels, 1u)} *
                                             std::max(desc.ArraySize, 1u));
    texture_layout(desc, placement.start, data.data(), static_cast<UINT>(data.size()));
    for (UINT index = 0; index < data.size(); ++index)
        context->UpdateSubresource(resource, index, nullptr, data[index].pSysMem,
                                   data[index].SysMemPitch, data[index].SysMemSlicePitch);
}
} // namespace

void track_placement(ID3D11Resource *resource, const void *address, UINT tile_mode, UINT pitch) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto bytes = resource ? resource_bytes(resource) : 0;
    if (!address || !bytes || !readable(start, bytes))
        return;
    const std::lock_guard lock(mutex);
    if (!handler)
        handler = AddVectoredExceptionHandler(1, &written);
    // a resource placed where an older one was replaces it
    if (const auto old = placements.find(start); old != placements.end()) {
        protect(old->second, PAGE_READWRITE); // the new one may not cover all of it
        old->second.resource->Release();
        placements.erase(old);
    }
    resource->AddRef();
    largest = std::max<std::uintptr_t>(largest, bytes);
    // a tiled texture was created from its memory as if it were linear: read it properly
    // before its first use
    const auto &placement =
        placements[start] = {resource, start, start + bytes, tile_mode > 1, tile_mode, pitch};
    if (placement.dirty)
        dirty.push_back(start);
    protect(placement, PAGE_READONLY);
}

bool placement_data(const D3D11_TEXTURE2D_DESC &desc, const void *address,
                    D3D11_SUBRESOURCE_DATA *data, UINT count) {
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    return address && desc.SampleDesc.Count <= 1 &&
           readable(start, texture_layout(desc, start, data, count));
}

void prepare_title_write(const void *address, std::size_t bytes) {
    const auto first = reinterpret_cast<std::uintptr_t>(address), last = first + bytes;
    if (!address || !bytes)
        return;
    const std::lock_guard lock(mutex);
    for (auto at = placements.lower_bound(last); at != placements.begin();) {
        auto &placement = (--at)->second;
        if (placement.start + largest < first)
            break;
        if (placement.end <= first || placement.start >= last)
            continue;
        if (!placement.dirty)
            dirty.push_back(placement.start);
        placement.dirty = true;
        protect(placement, PAGE_READWRITE);
    }
}

bool readable_memory(std::uint64_t address, std::size_t bytes) {
    return readable(static_cast<std::uintptr_t>(address), bytes);
}

void upload_placements(ID3D11DeviceContext *context) {
    const std::lock_guard lock(mutex);
    for (const auto start : dirty) {
        const auto found = placements.find(start);
        if (found == placements.end() || !found->second.dirty)
            continue;
        // memory the title has let go of: what was in it is all the resource gets
        if (!readable(found->second.start, found->second.end - found->second.start)) {
            found->second.resource->Release();
            placements.erase(found);
            continue;
        }
        upload(context, found->second);
        found->second.dirty = false;
        protect(found->second, PAGE_READONLY);
    }
    dirty.clear();
}
} // namespace xs::runtime::durango::d3d11x
