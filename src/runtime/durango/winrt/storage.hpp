#pragma once
#include <string_view>

struct IInspectable;

namespace xs::runtime::durango::winrt {
// Activation factory of Windows.Xbox.Storage.ConnectedStorageSpace, null for other names.
// Connected storage is kept in files under runtime::save_directory(): a "machine" and a
// "user" space, one folder per container and one file per blob.
IInspectable *storage_class(std::wstring_view name);
} // namespace xs::runtime::durango::winrt
