#pragma once
#include "runtime/durango/d3d11x/wrap.hpp"
#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace xs::runtime::durango::d3d11x {
// What to do with one argument of a method. Argument 0 is the object itself.
struct Fix {
    enum Type : std::uint8_t {
        none,
        in,          // one of our objects, may be null
        in_array,    // array of our objects; its length is argument `other`
        out,         // T **result of a method that returns nothing
        out_checked, // T **result of a method that returns an HRESULT
        out_iid,     // void **result whose interface ID is argument `other`
        out_array,   // array of results of a method that returns nothing; length in `other`
    } type = none;
    std::uint8_t argument = 0;
    std::uint8_t other = 0;
    Kind kind = Kind::child;
};
struct Method {
    enum Action : std::uint8_t {
        unknown, // nobody knows what is here: report the call and fail
        forward, // call slot `host` of the host object
        ignore,  // console-only and without effect here
        custom,  // `function` goes into the slot as it is
    } action = unknown;
    std::uint16_t host = 0;
    std::array<Fix, 3> fixes{};
    void *function = nullptr;
    bool draws = false; // needs the state the title left in the object applied first
};
inline constexpr std::size_t vtable_slots = 256, object_slots = 512;
struct Layout {
    const char *name;
    // size of the object in pointers; more than 1 only if the title reaches into it
    std::size_t words = 1;
    std::array<Method, vtable_slots> vtable{};
    // methods called through pointers stored in the object, by offset / 8
    std::array<Method, object_slots> object{};
    // applies state the title wrote into the object instead of calling a method
    void (*apply_state)(void *object) = nullptr;
};
const Layout &layout(Kind kind);
// Whether the title was built with the first development kits, whose DXGI interfaces have
// one more method than later ones: swap chains made from then on have their layout
void first_development_kits(bool yes);
bool first_development_kits();
// IDXGISwapChain::Present of a swap chain of ours
long present_swap_chain(void *swap_chain, unsigned interval); // HRESULT
// a Direct3D 12 resource handed to the title: the console's methods where they differ
void console_d3d12_resource(void *resource);
// why the host's Direct3D 12 device stopped, and what its debug layer said, to the log
void report_d3d12_failure();
// a Direct3D 12 texture's first subresource, copied to the CPU through the title's direct
// queue (XS_D3D_PROBE)
bool read_back_d3d12(ID3D12Resource *resource, std::vector<std::uint8_t> &data, unsigned &pitch,
                     unsigned &width, unsigned &height, DXGI_FORMAT &format,
                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT);
// the render target the title bound last, for XS_D3D_PROBE
ID3D12Resource *last_render_target();
// the first render target of each size and format the title bound
std::vector<ID3D12Resource *> bound_render_targets();
// how many draws went into a render target so far
std::uint64_t draws_into_target(ID3D12Resource *target);
} // namespace xs::runtime::durango::d3d11x
