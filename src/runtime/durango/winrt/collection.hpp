#pragma once
#include <vector>

struct IInspectable;

namespace xv::runtime::durango::winrt {
// A read-only IVectorView<T> of objects. Takes over the caller's reference to each item.
// The interface ID of IVectorView<T> depends on T, so every interface asked for is taken
// to be the view itself; IIterable<T> is not there.
void *make_vector_view(std::vector<IInspectable *> items);
// An empty IVector<T> of objects that the caller fills, answering every interface the
// same way. GetView gives a view of what it holds at the time.
void *make_vector();
} // namespace xv::runtime::durango::winrt
