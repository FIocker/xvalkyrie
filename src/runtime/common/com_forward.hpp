#pragma once
#include <cstddef>
#include <initializer_list>
#include <span>
#include <unknwn.h>

namespace xv::runtime {
// A COM object that hands every method to the same method of a host object, except for
// QueryInterface and the slots it is told to answer itself. For interfaces the host
// already implements the way a title expects, bar a few methods.
struct Forwarder;
struct SlotOverride {
    std::size_t slot;
    void *function; // same signature as the method; `this` is the Forwarder
};
using ForwarderQuery = HRESULT (*)(Forwarder *self, REFIID iid, void **object);
// Takes over the caller's reference to `host`. `slots` is the size of the host's vtable.
// The result is returned with one reference. `inserted`, when not 0, is a slot where the
// title's interface has one method more than the host's: it does nothing, and the host's
// methods from there on, and the overrides for them, are one slot later.
Forwarder *make_forwarder(IUnknown *host, std::size_t slots, ForwarderQuery query,
                          std::span<const SlotOverride> overrides, std::size_t inserted = 0);
inline Forwarder *make_forwarder(IUnknown *host, std::size_t slots, ForwarderQuery query,
                                 std::initializer_list<SlotOverride> overrides,
                                 std::size_t inserted = 0) {
    return make_forwarder(host, slots, query,
                          std::span<const SlotOverride>(overrides.begin(), overrides.size()),
                          inserted);
}
IUnknown *forwarder_host(Forwarder *self);
ULONG forwarder_add_ref(Forwarder *self);
// `object` itself unless it is a live Forwarder, then its host
IUnknown *unwrap_forwarder(IUnknown *object);
} // namespace xv::runtime
