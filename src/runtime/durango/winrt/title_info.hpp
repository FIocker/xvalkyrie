#pragma once
#include <string>
#include <string_view>

namespace xs::runtime::durango::winrt {
// What the console knows about the running title from its package manifest, which is in
// the title's folder (the current directory while it runs):
//   title_id           the Xbox Live title ID, in decimal as the console gives it
//   service_config_id  the primary service configuration ID
//   sandbox_id         always RETAIL
// Empty for what the manifest does not say.
std::wstring title_info(std::wstring_view key);
} // namespace xs::runtime::durango::winrt
