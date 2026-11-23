#pragma once

namespace xv::runtime {
class Registry;
namespace modules {
// XV_NETWORK=0: a title that reaches nothing outside. Its WinHTTP session and its sockets
// are refused where they are made, for the executable and for the DLLs it brings.
void install_network(Registry &registry);
} // namespace modules
} // namespace xv::runtime
