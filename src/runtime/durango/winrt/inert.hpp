#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <guiddef.h>

namespace xs::runtime::durango::winrt {
// What a method of a console interface does when the feature behind it is absent.
// Signatures come from how titles call them: the arguments passed and the size of the
// result read back.
enum class Inert : std::uint8_t {
    accept,         // takes its arguments and succeeds: setters, event removal
    zero8,          // HRESULT get(T *result) with a 1-byte result
    true8,          // same, result is true
    zero32,         // same, 4 bytes
    zero64,         // same, 8 bytes: null object, empty string
    object,         // HRESULT get(T **result) returning an inert object of type `result`
    lookup,         // HRESULT get(key, T **result) returning an inert object of type `result`
    lookup2,        // HRESULT get(key, key, T **result), likewise
    operation2,     // HRESULT start(argument, IAsyncOperation<T> **), finished at once likewise
    action2,        // HRESULT start(argument, IAsyncAction **), finished at once
    action3,        // HRESULT start(argument, argument, IAsyncAction **), finished at once
    not_stored2,    // HRESULT read(what, IAsyncAction **): fails at once, there is no such blob
    list_operation, // HRESULT start(IAsyncOperation<IVectorView<T>> **): finished, nothing found
    offline,        // HRESULT start(..., IAsyncOperation<T> **) of an online service: the operation
                    // fails at once. The result is argument number `argument`.
    action_at,      // HRESULT start(..., IAsyncAction **), finished at once; result in `argument`
    operation_at,   // like operation, with the result in argument number `argument`
    operation,      // HRESULT start(IAsyncOperation<T> **): finished at once with an inert `result`
    action,         // HRESULT start(IAsyncAction **): finished at once
    one32,          // HRESULT get(T *result) with a 4-byte result of 1
    one64,          // same, 8 bytes
    string,         // HRESULT get(HSTRING *result): the text in `result`
    shared,         // HRESULT get(T **result): the one object of type `result` there is
    shared_list,    // HRESULT get(IVectorView<T> **): a view with that one object
    empty_list,     // HRESULT get(IVectorView<T> **): an empty view
    empty_list2,    // HRESULT find(argument, IVectorView<T> **): nothing found
    one_item,       // HRESULT get(IVectorView<T> **): a view with one inert object of type `result`
    no_items,       // IVectorView::GetMany of an empty collection
    event_add,      // HRESULT add(handler, EventRegistrationToken *token); never raised
};
struct InertMethod {
    Inert kind;
    const wchar_t *result = nullptr; // type or text, for the kinds that have one
    unsigned argument = 0;           // Inert::offline: which argument is the result, from 1
};
// The interface a C++/CX caller uses without asking for it: the default interface of an
// object it was handed.
inline constexpr GUID direct{
    0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
// null if nothing is known about this method of `type`. Methods are numbered from
// 0 = QueryInterface.
const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method);
} // namespace xs::runtime::durango::winrt
