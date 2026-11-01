#pragma once
#include <cstddef>
#include <initializer_list>
#include <span>
#include <unknwn.h>

namespace xs::runtime {
// A COM object that hands every method to the same method of a host object, except for
// QueryInterface and the slots it is told to answer itself. For interfaces the host
// already implements the way a title expects, bar a few methods.
struct Forwarder;
struct SlotOverride {
    std::size_t slot;
    void *function; // same signature as the method; `this` is the Forwarder
};
using ForwarderQuery = HRESULT (*)(Forwarder *self, REFIID iid, void **object);
// Takes over the caller's reference to `host`. `slots` is the size of the vtable.
// The result is returned with one reference.
Forwarder *make_forwarder(IUnknown *host, std::size_t slots, ForwarderQuery query,
                          std::span<const SlotOverride> overrides);
inline Forwarder *make_forwarder(IUnknown *host, std::size_t slots, ForwarderQuery query,
                                 std::initializer_list<SlotOverride> overrides) {
    return make_forwarder(host, slots, query,
                          std::span<const SlotOverride>(overrides.begin(), overrides.size()));
}
IUnknown *forwarder_host(Forwarder *self);
ULONG forwarder_add_ref(Forwarder *self);
// `object` itself unless it is a live Forwarder, then its host
IUnknown *unwrap_forwarder(IUnknown *object);
} // namespace xs::runtime
