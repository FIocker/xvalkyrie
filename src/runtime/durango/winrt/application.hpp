#pragma once
#include <string_view>

struct IInspectable;
namespace xv::surface {
class TitleWindow;
}

namespace xv::runtime::durango::winrt {
// Activation factory of an application model class, null for other names.
// Windows.ApplicationModel.Core.CoreApplication runs the title's view on the calling thread
// with a CoreWindow backed by a host window; Windows.UI.Core.CoreWindow hands that window out.
IInspectable *application_class(std::wstring_view name);
// host window of the CoreWindow, null before CoreApplication::Run
surface::TitleWindow *current_title_window();
} // namespace xv::runtime::durango::winrt
