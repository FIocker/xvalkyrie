#pragma once
#include <guiddef.h>

namespace xs::runtime::durango::winrt {
// Stands in for a WinRT object nobody has implemented. Answers every QueryInterface, and
// every method past IInspectable reports who called it and fails with E_NOTIMPL, which is
// how the next missing piece gets found. `name` must outlive the object.
void *make_placeholder(const wchar_t *name, const GUID &iid);
// The one placeholder of class `name` that stands for something there is exactly one of
// (the signed-in user). Returned with a reference; the same object every time.
void *shared_placeholder(const wchar_t *name);
// QueryInterface of an object of class `name` for an interface it does not implement:
// a placeholder for it, or E_NOINTERFACE for the few interfaces COM itself probes for.
long placeholder_interface(const wchar_t *name, const GUID &iid, void **object);
// For a method of one of our classes that nobody knows the meaning of yet: reports it like
// a placeholder does and fails. `caller` is the return address into the title.
long unknown_method(const wchar_t *type, const char *iid, unsigned method, void *first,
                    void *second, void *caller);
} // namespace xs::runtime::durango::winrt
