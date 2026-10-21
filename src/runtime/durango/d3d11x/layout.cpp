#include "runtime/durango/d3d11x/layout.hpp"
#include "backends/graphics/d3d11/host.hpp"
#include "backends/surface/surface.hpp"
#include "runtime/durango/winrt/application.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <initializer_list>
#include <iterator>

// Where each method is. Host slots are the desktop vtable indices of d3d11_1.h; the
// console positions come from how titles call them.
namespace xs::runtime::durango::d3d11x {
void *unwrap(void *object);
namespace {
constexpr Fix in(std::uint8_t argument) { return {Fix::in, argument}; }
constexpr Fix in_array(std::uint8_t argument, std::uint8_t count) {
    return {Fix::in_array, argument, count};
}
constexpr Fix out(std::uint8_t argument, Kind kind) { return {Fix::out, argument, 0, kind}; }
constexpr Fix created(std::uint8_t argument, Kind kind) {
    return {Fix::out_checked, argument, 0, kind};
}
constexpr Fix out_array(std::uint8_t argument, std::uint8_t count, Kind kind) {
    return {Fix::out_array, argument, count, kind};
}
constexpr Fix out_iid(std::uint8_t argument, std::uint8_t iid) {
    return {Fix::out_iid, argument, iid};
}
Method forward(std::size_t host, std::initializer_list<Fix> fixes = {}) {
    Method method{Method::forward, static_cast<std::uint16_t>(host)};
    std::size_t index = 0;
    for (const auto &fix : fixes)
        method.fixes[index++] = fix;
    return method;
}
Method custom(auto *function) {
    return {Method::custom, 0, {}, reinterpret_cast<void *>(function)};
}

// ID3D11DeviceChild: the four desktop methods, then two console ones (private data for
// graphics interfaces, object name).
void device_child(Layout &result) {
    result.vtable[3] = forward(3, {out(1, Kind::device)}); // GetDevice
    for (std::size_t slot = 4; slot <= 6; ++slot)
        result.vtable[slot] = forward(slot);
    result.vtable[7] = result.vtable[8] = {Method::ignore};
}
// A device child whose own methods (host slots 7 to `last`) follow at 9
Layout child(const char *name, std::size_t last) {
    Layout result{name};
    device_child(result);
    for (std::size_t host = 7; host <= last; ++host)
        result.vtable[host + 2] = forward(host);
    return result;
}
// ID3D11Resource: three desktop methods at 9, a console one at 12, then the type's own
Layout resource(const char *name, std::size_t last) {
    Layout result{name};
    device_child(result);
    for (std::size_t host = 7; host <= 9; ++host)
        result.vtable[host + 2] = forward(host);
    for (std::size_t host = 10; host <= last; ++host)
        result.vtable[host + 3] = forward(host);
    return result;
}
Layout view() {
    auto result = child("view", 8);
    result.vtable[9] = forward(7, {out(1, Kind::resource)}); // GetResource
    return result;
}

constexpr std::size_t extra_device_slot = 40;

// CreateTexture2D: the console description has flags and values the host rejects
HRESULT STDMETHODCALLTYPE create_texture2d(void *self, const D3D11_TEXTURE2D_DESC *description,
                                           const D3D11_SUBRESOURCE_DATA *initial,
                                           ID3D11Texture2D **texture) {
    if (!description)
        return E_INVALIDARG;
    auto host = *description;
    // generate mips, cube, indirect arguments, raw and structured buffers, clamp
    host.MiscFlags &= 0x1 | 0x4 | 0x10 | 0x20 | 0x40 | 0x80;
    if (host.SampleDesc.Count <= 1)
        host.SampleDesc = {1, 0};
    // the console lets the CPU read a dynamic texture too
    if (host.Usage == D3D11_USAGE_DYNAMIC)
        host.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateTexture2D(&host, initial, texture ? &created : nullptr);
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreateTexture2D %ux%u mips %u array %u format %u usage %u bind "
                     "0x%x cpu 0x%x misc 0x%x: 0x%08lx\n",
                     host.Width, host.Height, host.MipLevels, host.ArraySize,
                     static_cast<unsigned>(host.Format), static_cast<unsigned>(host.Usage),
                     host.BindFlags, host.CPUAccessFlags, description->MiscFlags,
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    } else if (texture) {
        *texture = static_cast<ID3D11Texture2D *>(wrap(created, Kind::texture2d));
    }
    // every format once: what the title draws with
    static std::atomic<std::uint64_t> seen[3]{};
    const auto format = static_cast<unsigned>(host.Format);
    if (format < 192 &&
        !(seen[format / 64].fetch_or(1ull << (format % 64)) & (1ull << (format % 64)))) {
        std::fprintf(stderr, "[Runtime] first 2D texture of format %u: %ux%u usage %u bind 0x%x\n",
                     format, host.Width, host.Height, static_cast<unsigned>(host.Usage),
                     host.BindFlags);
        std::fflush(stderr);
    }
    return result;
}

// CreateShaderResourceView, which says what it was given when the host refuses it
HRESULT STDMETHODCALLTYPE create_shader_resource_view(void *self, void *resource,
                                                      const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                                                      ID3D11ShaderResourceView **view) {
    auto *const host = static_cast<ID3D11Resource *>(unwrap(resource));
    ID3D11ShaderResourceView *created = nullptr;
    const auto result = static_cast<ID3D11Device *>(unwrap(self))
                            ->CreateShaderResourceView(host, desc, view ? &created : nullptr);
    if (FAILED(result)) {
        D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (host)
            host->GetType(&dimension);
        std::fprintf(
            stderr,
            "[Runtime] CreateShaderResourceView of a resource of dimension %u: format %u "
            "view dimension %u, words %08x %08x %08x %08x: 0x%08lx\n",
            static_cast<unsigned>(dimension), desc ? static_cast<unsigned>(desc->Format) : 0u,
            desc ? static_cast<unsigned>(desc->ViewDimension) : 0u,
            desc ? desc->Buffer.FirstElement : 0u, desc ? desc->Buffer.NumElements : 0u,
            desc ? desc->Texture2DArray.FirstArraySlice : 0u,
            desc ? desc->Texture2DArray.ArraySize : 0u, static_cast<unsigned long>(result));
        std::fflush(stderr);
    } else if (view) {
        *view = static_cast<ID3D11ShaderResourceView *>(wrap(created, Kind::view));
    }
    return result;
}

Layout device() {
    Layout result{"device"};
    const auto set = [&](std::size_t slot, std::initializer_list<Fix> fixes = {}) {
        result.vtable[slot] = forward(slot, fixes);
    };
    for (std::size_t slot = 3; slot <= 49; ++slot)
        set(slot);
    set(3, {created(3, Kind::buffer)});
    set(4, {created(3, Kind::texture1d)});
    result.vtable[5] = custom(&create_texture2d);
    set(6, {created(3, Kind::texture3d)});
    for (std::size_t slot = 7; slot <= 10; ++slot) // views of a resource
        set(slot, {in(1), created(3, Kind::view)});
    result.vtable[7] = custom(&create_shader_resource_view);
    set(11, {created(5, Kind::child)});               // input layout
    for (std::size_t slot : {12, 13, 15, 16, 17, 18}) // shaders
        set(slot, {in(3), created(4, Kind::child)});
    set(14, {in(8), created(9, Kind::child)});      // geometry shader with stream output
    set(19, {created(1, Kind::state)});             // class linkage
    for (std::size_t slot = 20; slot <= 26; ++slot) // states, queries, counters
        set(slot, {created(2, Kind::state)});
    set(27, {created(2, Kind::context)});
    set(40, {out(1, Kind::context)});
    set(43, {out(1, Kind::context)});
    set(44, {created(2, Kind::context)});
    set(45, {created(2, Kind::state)});
    set(46, {created(2, Kind::state)});
    // shared resources and context states are not something a console title has
    for (std::size_t slot : {28, 47, 48, 49})
        result.vtable[slot] = {};
    // The console device has one method more than the desktop one somewhere before
    // GetImmediateContext, which titles call at 41 instead of 40. Until a call shows where,
    // it is taken to be right before it.
    for (std::size_t slot = 50; slot > extra_device_slot; --slot)
        result.vtable[slot] = result.vtable[slot - 1];
    result.vtable[extra_device_slot] = {};
    // Console only: fills 16 bytes the title cleared, and takes another path if the second
    // half says 2 or more. Left as it is.
    result.vtable[85] = {Method::ignore};
    return result;
}

// what follows each frame the title presents, whichever way it does
void presented() {
    if (auto *const window = winrt::current_title_window()) {
        window->show();
        static std::atomic<int> frames{0};
        if (++frames == 120) { // once it has had time to appear
            std::fprintf(stderr, "[Runtime] title surface %s\n", window->describe().c_str());
            std::fflush(stderr);
        }
    }
}
HRESULT STDMETHODCALLTYPE present(void *self, UINT interval, UINT flags) {
    auto *const host = static_cast<IDXGISwapChain *>(unwrap(self));
    graphics::d3d11::frame_presented(host);
    const auto result = host->Present(interval, flags);
    presented();
    return result;
}
// Present1: the console takes no parameters to mean the whole frame, the host crashes
HRESULT STDMETHODCALLTYPE present1(void *self, UINT interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS *parameters) {
    auto *const host = static_cast<IDXGISwapChain1 *>(unwrap(self));
    graphics::d3d11::frame_presented(host);
    const DXGI_PRESENT_PARAMETERS whole{};
    const auto result = host->Present1(interval, flags, parameters ? parameters : &whole);
    presented();
    return result;
}

Layout swap_chain() {
    Layout result{"swap chain"};
    for (std::size_t slot = 3; slot <= 28; ++slot)
        result.vtable[slot] = forward(slot);
    result.vtable[7] = forward(7, {out_iid(2, 1)}); // GetDevice
    result.vtable[8] = custom(&present);
    result.vtable[22] = custom(&present1);
    result.vtable[9] = forward(9, {out_iid(3, 2)}); // GetBuffer
    return result;
}

// Methods with a float argument or result, which the generic path does not carry
void STDMETHODCALLTYPE clear_depth_stencil(void *self, void *view, UINT flags, FLOAT depth,
                                           UINT8 stencil) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->ClearDepthStencilView(static_cast<ID3D11DepthStencilView *>(unwrap(view)), flags, depth,
                                stencil);
}
void STDMETHODCALLTYPE set_min_lod(void *self, void *resource, FLOAT lod) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->SetResourceMinLOD(static_cast<ID3D11Resource *>(unwrap(resource)), lod);
}
FLOAT STDMETHODCALLTYPE get_min_lod(void *self, void *resource) {
    return static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->GetResourceMinLOD(static_cast<ID3D11Resource *>(unwrap(resource)));
}

// XXSetShaderResources through the in-object table: one view per call, passed as
// (address of the view pointer, slot, a hardware packet header that means nothing here).
template <void (STDMETHODCALLTYPE ID3D11DeviceContext::*Set)(UINT, UINT,
                                                             ID3D11ShaderResourceView *const *)>
void STDMETHODCALLTYPE set_shader_resource(void *self, void **view, UINT slot, UINT) {
    auto *const host = static_cast<ID3D11ShaderResourceView *>(view ? unwrap(*view) : nullptr);
    (static_cast<ID3D11DeviceContext *>(unwrap(self))->*Set)(slot, 1, &host);
}

// IASetIndexBuffer through the in-object table takes the index size first, as the
// hardware names it: 0 for 16-bit indices, 1 for 32-bit ones.
void STDMETHODCALLTYPE set_index_buffer(void *self, UINT hardware_format, void *buffer,
                                        UINT offset) {
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->IASetIndexBuffer(static_cast<ID3D11Buffer *>(unwrap(buffer)),
                           hardware_format ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, offset);
}

// State a title sets without calling anything: the console headers write the input layout,
// the vertex and pixel shaders and the primitive topology into the context object and mark
// them dirty; the console applies them at the next draw. So do we.
namespace state {
constexpr std::size_t dirty = 0x168, topology = 0x170, input_layout = 0x178, vertex_shader = 0x180,
                      pixel_shader = 0x188;
// What the headers store for each D3D11_PRIMITIVE_TOPOLOGY, indexed by it: a hardware
// register value. Entries equal to the first one are topologies that do not exist.
constexpr std::uint64_t topologies[] = {
    0x1ffc0009e00,  0x3ffc0009e00,  0x5ffc0009e00,  0x7ffc0009e00, 0x9ffc0009e00,
    0xdffc0009e00,  0xbffc0009e00,  0x1ffc0009e00,  0x1ffc0009e00, 0x1ffc0009e00,
    0x157fc0009e00, 0x177fc0009e00, 0x197fc0009e00, 0x1b7fc0009e00};
template <class T> T read(void *object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<char *>(object) + offset, sizeof value);
    return value;
}
} // namespace state
void apply_context_state(void *self) {
    if (!state::read<std::uint32_t>(self, state::dirty))
        return;
    const std::uint32_t clean = 0;
    std::memcpy(static_cast<char *>(self) + state::dirty, &clean, sizeof clean);
    auto *const host = static_cast<ID3D11DeviceContext *>(unwrap(self));
    host->IASetInputLayout(
        static_cast<ID3D11InputLayout *>(unwrap(state::read<void *>(self, state::input_layout))));
    host->VSSetShader(
        static_cast<ID3D11VertexShader *>(unwrap(state::read<void *>(self, state::vertex_shader))),
        nullptr, 0);
    host->PSSetShader(
        static_cast<ID3D11PixelShader *>(unwrap(state::read<void *>(self, state::pixel_shader))),
        nullptr, 0);
    const auto value = state::read<std::uint64_t>(self, state::topology);
    for (std::size_t topology = 1; topology < std::size(state::topologies); ++topology)
        if (state::topologies[topology] == value && value != state::topologies[0]) {
            host->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(topology));
            break;
        }
}
// DrawIndexed through the in-object table: (index count << 32 | first index, base vertex)
void STDMETHODCALLTYPE draw_indexed(void *self, UINT64 count_and_start, INT base_vertex) {
    apply_context_state(self);
    static_cast<ID3D11DeviceContext *>(unwrap(self))
        ->DrawIndexed(static_cast<UINT>(count_and_start >> 32), static_cast<UINT>(count_and_start),
                      base_vertex);
}

// Desktop slot of every ID3D11DeviceContext1 method, with what its arguments are.
std::array<Method, 134> context_methods() {
    std::array<Method, 134> methods{};
    const auto set = [&](std::size_t slot, std::initializer_list<Fix> fixes = {}) {
        methods[slot] = forward(slot, fixes);
    };
    set(3, {out(1, Kind::device)});
    for (std::size_t slot = 4; slot <= 6; ++slot)
        set(slot);
    // per stage: constant buffers, shader resources, samplers (start, count, array)
    for (std::size_t slot : {7, 16, 22, 62, 66, 71, 8, 25, 31, 59, 63, 67, 10, 26, 32, 61, 65, 70})
        set(slot, {in_array(3, 2)});
    for (std::size_t slot : {9, 11, 23, 60, 64, 69}) // shader, class instances
        set(slot, {in(1), in_array(2, 3)});
    for (std::size_t slot : {12, 13, 20, 21, 24, 38, 41, 44, 45, 110, 111, 112, 113})
        set(slot); // draws, topology, viewports, scissors, ClearState, Flush
    for (std::size_t slot : {12, 13, 20, 21, 38})
        methods[slot].draws = true;
    set(14, {in(1)}); // Map
    set(15, {in(1)}); // Unmap
    set(17, {in(1)}); // IASetInputLayout
    set(18, {in_array(3, 2)});
    set(19, {in(1)});
    for (std::size_t slot : {27, 28, 29, 30}) // Begin, End, GetData, SetPredication
        set(slot, {in(1)});
    set(33, {in_array(2, 1), in(3)}); // OMSetRenderTargets
    set(34, {in_array(2, 1), in(3), in_array(6, 5)});
    set(35, {in(1)});
    set(36, {in(1)});
    set(37, {in_array(2, 1)});
    for (std::size_t slot : {39, 40, 42, 43}) // indirect draws, RSSetState
        set(slot, {in(1)});
    methods[39].draws = methods[40].draws = true;
    set(46, {in(1), in(6)}); // CopySubresourceRegion
    set(47, {in(1), in(2)}); // CopyResource
    set(48, {in(1)});        // UpdateSubresource
    set(49, {in(1), in(3)});
    for (std::size_t slot : {50, 51, 52, 54}) // clears, GenerateMips
        set(slot, {in(1)});
    methods[53] = custom(&clear_depth_stencil);
    methods[55] = custom(&set_min_lod);
    methods[56] = custom(&get_min_lod);
    set(57, {in(1), in(3)}); // ResolveSubresource
    set(58, {in(1)});        // ExecuteCommandList
    set(68, {in_array(3, 2)});
    // what is bound, per stage: constant buffers, shader resources, samplers, shader
    for (std::size_t slot : {72, 77, 81, 100, 104, 109, 125, 126, 127, 128, 129, 130})
        set(slot, {out_array(3, 2, Kind::buffer)});
    for (std::size_t slot : {73, 84, 87, 97, 101, 105, 106})
        set(slot, {out_array(3, 2, Kind::view)});
    for (std::size_t slot : {75, 85, 88, 99, 103, 108})
        set(slot, {out_array(3, 2, Kind::state)});
    for (std::size_t slot : {74, 76, 82, 98, 102, 107})
        set(slot, {out(1, Kind::child)});
    set(78, {out(1, Kind::child)});                             // IAGetInputLayout
    set(79, {out_array(3, 2, Kind::buffer)});                   // IAGetVertexBuffers
    set(80, {out(1, Kind::buffer)});                            // IAGetIndexBuffer
    set(83);                                                    // IAGetPrimitiveTopology
    set(86, {out(1, Kind::state)});                             // GetPredication
    set(89, {out_array(2, 1, Kind::view), out(3, Kind::view)}); // OMGetRenderTargets
    set(90, {out_array(2, 1, Kind::view), out(3, Kind::view), out_array(6, 5, Kind::view)});
    set(91, {out(1, Kind::state)});
    set(92, {out(1, Kind::state)});
    set(93, {out_array(2, 1, Kind::buffer)}); // SOGetTargets
    set(94, {out(1, Kind::state)});
    set(95);
    set(96);
    set(114, {created(2, Kind::state)}); // FinishCommandList
    set(115, {in(1), in(6)});
    set(116, {in(1)});
    set(117, {in(1)});
    set(118, {in(1)});
    for (std::size_t slot = 119; slot <= 124; ++slot) // constant buffers with ranges
        set(slot, {in_array(3, 2)});
    set(132, {in(1)});
    set(133, {in(1)});
    return methods;
}
// The context has its methods twice: in the vtable, two slots later than on the desktop
// like every device child, and as pointers inside the object, which is how titles call
// most of them. That table is in desktop order with VSSetConstantBuffers (desktop 7) at
// 131, so desktop + 124, up to somewhere before VSSetShaderResources (desktop 25), which
// is at 148 like everything after it: desktop + 123. One of the methods from 16 to 24 is
// not there; until a call shows which, the last one loses its place.
Layout context() {
    constexpr std::size_t object_table = 124;
    Layout result{"context", object_slots};
    device_child(result);
    const auto methods = context_methods();
    for (std::size_t host = 7; host < methods.size(); ++host)
        result.vtable[host + 2] = methods[host];
    for (std::size_t host = 3; host < methods.size(); ++host)
        result.object[host + object_table - (host >= 25)] = methods[host];
    using Context = ID3D11DeviceContext;
    result.object[8 + object_table] = custom(&set_shader_resource<&Context::PSSetShaderResources>);
    result.object[19 + object_table] = custom(&set_index_buffer);
    result.object[12 + object_table] = custom(&draw_indexed);
    result.apply_state = &apply_context_state;
    result.object[25 + 123] = custom(&set_shader_resource<&Context::VSSetShaderResources>);
    result.object[31 + 123] = custom(&set_shader_resource<&Context::GSSetShaderResources>);
    result.object[59 + 123] = custom(&set_shader_resource<&Context::HSSetShaderResources>);
    result.object[63 + 123] = custom(&set_shader_resource<&Context::DSSetShaderResources>);
    result.object[67 + 123] = custom(&set_shader_resource<&Context::CSSetShaderResources>);
    return result;
}
} // namespace

const Layout &layout(Kind kind) {
    static const Layout layouts[] = {
        device(),
        context(),
        swap_chain(),
        resource("buffer", 10),
        resource("texture 1D", 10),
        resource("texture 2D", 10),
        resource("texture 3D", 10),
        resource("resource", 9),
        view(),
        child("state", 8),
        child("device child", 6),
    };
    return layouts[static_cast<std::size_t>(kind)];
}

// Console-only interface IDs, as the system's d3d11_x.dll exports them. Titles use the
// "performance" ones as the device and the context themselves.
constexpr GUID device_x{
    0x177700f9, 0x876a, 0x4436, {0xb3, 0x68, 0x36, 0xa6, 0x04, 0xf8, 0x2c, 0xef}};
constexpr GUID performance_device_x{
    0x88671610, 0x712e, 0x4f1e, {0x84, 0xab, 0x01, 0xb5, 0x94, 0x8b, 0xd3, 0x73}};
constexpr GUID context_x{
    0x48800095, 0x7134, 0x4be7, {0x91, 0x86, 0xb8, 0x6b, 0xec, 0xb2, 0x64, 0x77}};
constexpr GUID performance_context_x{
    0x9458fe06, 0xc78d, 0x47f7, {0x96, 0xa0, 0xec, 0x7b, 0x72, 0x7b, 0xe1, 0xe9}};

bool kind_of(const GUID &iid, Kind &kind) {
    if (iid == __uuidof(ID3D11Texture2D))
        kind = Kind::texture2d;
    else if (iid == __uuidof(ID3D11Texture1D))
        kind = Kind::texture1d;
    else if (iid == __uuidof(ID3D11Texture3D))
        kind = Kind::texture3d;
    else if (iid == __uuidof(ID3D11Buffer))
        kind = Kind::buffer;
    else if (iid == __uuidof(ID3D11Resource))
        kind = Kind::resource;
    else if (iid == __uuidof(ID3D11Device) || iid == __uuidof(ID3D11Device1) || iid == device_x ||
             iid == performance_device_x)
        kind = Kind::device;
    else if (iid == __uuidof(ID3D11DeviceContext) || iid == __uuidof(ID3D11DeviceContext1) ||
             iid == context_x || iid == performance_context_x)
        kind = Kind::context;
    else if (iid == __uuidof(IDXGISwapChain) || iid == __uuidof(IDXGISwapChain1))
        kind = Kind::swap_chain;
    else
        return false;
    return true;
}
} // namespace xs::runtime::durango::d3d11x
