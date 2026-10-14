#pragma once

struct IUnknown;

namespace xs::runtime::durango::winrt {
// An IAsyncOperation<T> that has already finished with `result`, taking over the caller's
// reference to it. T is an object or a string; other result types need their own class.
// A Completed handler runs as soon as it is set, as WinRT does for finished operations.
void *make_completed_operation(void *result);
// The same for IAsyncAction, which has no result.
void *make_completed_action();
// An IAsyncAction that has already failed with this HRESULT.
void *make_failed_action(long error);
// An IAsyncOperation<T> that has already failed with this HRESULT.
void *make_failed_operation(long error);
} // namespace xs::runtime::durango::winrt
