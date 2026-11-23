#include "runtime/modules/network.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <winsock2.h>

#include <windows.h>

// Everything a title sends goes through a WinHTTP session (WinHttpOpen, also for the
// libHttpClient and XCurl it ships) or a socket. With XV_NETWORK=0 neither is made, so
// nothing leaves the host: a title's crash reporter, telemetry and sign-in find no network.
namespace xv::runtime::modules {
namespace {
constexpr DWORD winhttp_cannot_connect = 12029; // ERROR_WINHTTP_CANNOT_CONNECT
void said_once() {
    static const bool said = [] {
        std::fprintf(stderr, "[Runtime] XV_NETWORK=0: the title's network calls are refused\n");
        std::fflush(stderr);
        return true;
    }();
    static_cast<void>(said);
}
void *WINAPI winhttp_open(const wchar_t *, DWORD, const wchar_t *, const wchar_t *, DWORD) {
    said_once();
    SetLastError(winhttp_cannot_connect);
    return nullptr;
}
SOCKET WINAPI no_socket(int, int, int) {
    said_once();
    WSASetLastError(WSAENETDOWN);
    return INVALID_SOCKET;
}
SOCKET WINAPI no_wsa_socket(int, int, int, void *, unsigned, DWORD) { return no_socket(0, 0, 0); }
hostent *WINAPI no_host_by_name(const char *) {
    WSASetLastError(WSAHOST_NOT_FOUND);
    return nullptr;
}
int WINAPI no_address_info(const void *, const void *, const void *, void **result) {
    if (result)
        *result = nullptr;
    return WSAHOST_NOT_FOUND;
}
} // namespace

void install_network(Registry &registry) {
    const auto *const setting = std::getenv("XV_NETWORK");
    if (!setting || std::strcmp(setting, "0") != 0)
        return;
    const auto erase = [](auto *function) { return reinterpret_cast<void *>(function); };
    registry.add("winhttp.dll", "WinHttpOpen", erase(&winhttp_open));
    // ws2_32.dll by name and by the ordinals its import library gives them
    for (const auto *name : {"socket", "#23"})
        registry.add("ws2_32.dll", name, erase(&no_socket));
    for (const auto *name : {"WSASocketW", "WSASocketA"})
        registry.add("ws2_32.dll", name, erase(&no_wsa_socket));
    for (const auto *name : {"gethostbyname", "#52"})
        registry.add("ws2_32.dll", name, erase(&no_host_by_name));
    for (const auto *name : {"getaddrinfo", "GetAddrInfoW"})
        registry.add("ws2_32.dll", name, erase(&no_address_info));
}
} // namespace xv::runtime::modules
