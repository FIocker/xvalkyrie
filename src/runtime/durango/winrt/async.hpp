#pragma once
#include <functional>
#include <string>

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
// The same for an IAsyncOperation<T> of a number: GetResults writes the `size` low bytes
// of `value`.
void *make_completed_value(unsigned long long value, unsigned size);
// An IAsyncOperation<T> that has already failed with this HRESULT.
void *make_failed_operation(long error);
// IAsyncOperationWithProgress<T, P>
void *make_failed_progress_operation(long error);
// IAsyncOperation<HSTRING> finished by the function `start` gets, from any thread
void *make_text_operation(std::function<void(std::function<void(std::wstring)>)> start);
} // namespace xs::runtime::durango::winrt
