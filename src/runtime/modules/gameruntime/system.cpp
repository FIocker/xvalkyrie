#include "formats/gdk/game_config.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>

// The small APIs of the Gaming Runtime: what the title is (XGame, XPackage), what it runs on
// (XSystem, XDisplay, XNetworking), where it may keep files (XPersistentLocalStorage), and the
// ones that only register for events the host never raises.
namespace xs::runtime::modules::gameruntime {
namespace {
std::atomic<std::uint64_t> tokens{0};
const char port_identity[] = "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync";

std::filesystem::path package_root() {
    std::error_code error;
    return package_directory(std::filesystem::current_path(error));
}
// MicrosoftGame.config of the running title, null if it has none that can be read
const nlohmann::json &game_config() {
    static const nlohmann::json config = [] {
        try {
            return gdk::inspect_config(
                host::read_snapshot(package_root() / "MicrosoftGame.config", gdk::config_limit));
        } catch (const std::exception &) {
            return nlohmann::json();
        }
    }();
    return config;
}
std::string config_text(const char *key) {
    const auto &config = game_config();
    return config.is_object() && config.contains(key) && config.at(key).is_string()
               ? config.at(key).get<std::string>()
               : std::string();
}
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

HRESULT WINAPI title_id(void *, std::uint32_t *id) {
    if (!id)
        return E_POINTER;
    *id = static_cast<std::uint32_t>(std::strtoul(config_text("title_id").c_str(), nullptr, 16));
    return S_OK;
}
void WINAPI launch_new_game(void *, const char *path, const char *, void *) {
    std::fprintf(stderr, "[Runtime] the title asks to start %s, which is not done\n",
                 path ? path : "(null)");
    std::fflush(stderr);
}
bool WINAPI feature_available(void *, std::uint32_t) { return true; }
void WINAPI set_error_callback(void *, void *, void *) {}
void WINAPI set_error_options(void *, std::uint32_t, std::uint32_t) {}
void WINAPI error_report(void *, HRESULT result, const char *message) {
    std::fprintf(stderr, "[Runtime] Gaming Runtime error 0x%08lx: %s\n",
                 static_cast<unsigned long>(result), message ? message : "");
    std::fflush(stderr);
}

// XSystemAnalyticsInfo: two versions, then the device's family and form
struct AnalyticsInfo {
    std::uint16_t os_version[4], hosting_os_version[4];
    char family[64], form[64];
};
AnalyticsInfo *WINAPI analytics_info(void *, AnalyticsInfo *info) {
    *info = {{10, 0, 19041, 0}, {10, 0, 19041, 0}, "Xbox", "Console"};
    return info;
}
// the console's id is 38 characters, the same every run
HRESULT WINAPI console_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text(std::string(38, '0'), size, buffer, used);
}
HRESULT WINAPI sandbox_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text("RETAIL", size, buffer, used);
}
HRESULT WINAPI app_device_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text(std::string(44, 'A'), size, buffer, used);
}
// XDisplayTryEnableHdrMode: the title surface is not an HDR one
std::uint32_t WINAPI try_enable_hdr(void *, std::uint32_t, float *info) {
    if (info)
        info[0] = info[1] = info[2] = 0;
    return 2; // disabled
}
HRESULT WINAPI timeout_deferral(void *, void **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &tokens;
    return S_OK;
}
void WINAPI close_handle(void *, void *) {}
HRESULT WINAPI launch_uri(void *, void *, const char *uri) {
    std::fprintf(stderr, "[Runtime] the title asks to open %s, which is not done\n",
                 uri ? uri : "(null)");
    std::fflush(stderr);
    return S_OK;
}

// XPersistentLocalStorage: a folder of the title's that outlives it, next to its saves
std::string local_storage() {
    auto root = save_directory();
    if (root.empty())
        root = std::filesystem::temp_directory_path() / "xscarlet-saves";
    const auto folder = root / "LocalStorage";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return utf8(folder);
}
HRESULT WINAPI storage_path_size(void *, std::size_t *size) {
    if (!size)
        return E_POINTER;
    *size = local_storage().size() + 1;
    return S_OK;
}
HRESULT WINAPI storage_path(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text(local_storage(), size, buffer, used);
}
// XPersistentLocalStorageSpaceInfo: available and used bytes
HRESULT WINAPI storage_space(void *, std::uint64_t *info) {
    if (!info)
        return E_POINTER;
    info[0] = 1ull << 30;
    info[1] = 0;
    return S_OK;
}

// XPackage: the title is one package, all of it installed, mounted where it was unpacked
std::string package_identifier() {
    const auto name = config_text("name");
    return name.empty() ? "XScarlet.Title_1.0.0.0_x64__0000000000000" : name + "_x64__title";
}
HRESULT WINAPI package_identifier_of_process(void *, std::size_t size, char *buffer) {
    return copy_text(package_identifier(), size, buffer, nullptr);
}
bool WINAPI is_packaged(void *) { return true; }
HRESULT WINAPI user_locale(void *, std::size_t size, char *buffer) {
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
    char text[LOCALE_NAME_MAX_LENGTH * 3] = "en-US";
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof text, nullptr, nullptr);
    return copy_text(text, size, buffer, nullptr);
}
// XPackageChunkAvailability: ready
HRESULT WINAPI chunk_availability(void *, const char *, std::uint32_t, void *,
                                  std::uint32_t *availability) {
    if (!availability)
        return E_POINTER;
    *availability = 0;
    return S_OK;
}
struct Mount {
} mount;
HRESULT WINAPI mount_package(void *, const char *, Mount **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &mount;
    return S_OK;
}
HRESULT WINAPI mount_path_size(void *, Mount *, std::size_t *size) {
    if (!size)
        return E_POINTER;
    *size = utf8(package_root()).size() + 1;
    return S_OK;
}
HRESULT WINAPI mount_path(void *, Mount *, std::size_t size, char *buffer) {
    return copy_text(utf8(package_root()), size, buffer, nullptr);
}
// no downloadable content is installed, and no features are split off
HRESULT WINAPI enumerate_nothing(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t) {
    return S_OK;
}

// XNetworking: connected, as far as a title needs to know before it tries
struct ConnectivityHint {
    std::uint32_t level, cost, interface_type;
    bool initialized, approaching_limit, over_limit, roaming;
};
HRESULT WINAPI connectivity_hint(void *, ConnectivityHint *hint) {
    if (!hint)
        return E_POINTER;
    // internet access, unrestricted, over Ethernet
    *hint = {4, 1, 6, true, false, false, false};
    return S_OK;
}
HRESULT WINAPI udp_port(void *, std::uint16_t *port) {
    if (!port)
        return E_POINTER;
    *port = 3074;
    return S_OK;
}
HRESULT WINAPI udp_port_async(void *, XAsyncBlock *block) {
    const std::uint16_t port = 3074;
    std::vector<std::uint8_t> payload(sizeof port);
    std::memcpy(payload.data(), &port, sizeof port);
    return finish(block, port_identity, S_OK, std::move(payload));
}
HRESULT WINAPI udp_port_result(void *, XAsyncBlock *block, std::uint16_t *port) {
    return result(block, port_identity, sizeof *port, port, nullptr);
}

// events the host never raises: a token for the registration, and nothing more. The token
// is the last argument of all of them.
HRESULT WINAPI register_3(void *, void *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI unregister(void *, std::uint64_t, bool) { return true; }
HRESULT WINAPI succeeded(void *) { return S_OK; }
bool WINAPI no(void *) { return false; }
} // namespace

void install_system() {
    implement("XGameGetXboxTitleId", &title_id);
    implement("XLaunchNewGame", &launch_new_game);
    implement("XGameRuntimeIsFeatureAvailable", &feature_available);
    implement("XErrorSetCallback", &set_error_callback);
    implement("XErrorSetOptions", &set_error_options);
    implement("XErrorReport", &error_report);
    implement("XSystemGetAnalyticsInfo", &analytics_info);
    implement("XSystemGetConsoleId", &console_id);
    implement("XSystemGetXboxLiveSandboxId", &sandbox_id);
    implement("XSystemGetAppSpecificDeviceId", &app_device_id);
    implement("XDisplayTryEnableHdrMode", &try_enable_hdr);
    implement("XDisplayAcquireTimeoutDeferral", &timeout_deferral);
    implement("XDisplayCloseTimeoutDeferralHandle", &close_handle);
    implement("XLaunchUri", &launch_uri);
    implement("XPersistentLocalStorageGetPathSize", &storage_path_size);
    implement("XPersistentLocalStorageGetPath", &storage_path);
    implement("XPersistentLocalStorageGetSpaceInfo", &storage_space);
    implement("XPackageGetCurrentProcessPackageIdentifier", &package_identifier_of_process);
    implement("XPackageIsPackagedProcess", &is_packaged);
    implement("XPackageGetUserLocale", &user_locale);
    implement("XPackageFindChunkAvailability", &chunk_availability);
    implement("XPackageMount", &mount_package);
    implement("XPackageGetMountPathSize", &mount_path_size);
    implement("XPackageGetMountPath", &mount_path);
    implement("XPackageCloseMountHandle", &close_handle);
    implement("XPackageEnumeratePackages", &enumerate_nothing);
    implement("XPackageEnumerateFeatures", &enumerate_nothing);
    implement("XPackageRegisterPackageInstalled", &register_3);
    implement("XPackageUnregisterPackageInstalled", &unregister);
    implement("XNetworkingGetConnectivityHint", &connectivity_hint);
    implement("XNetworkingRegisterConnectivityHintChanged", &register_3);
    implement("XNetworkingUnregisterConnectivityHintChanged", &unregister);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPort", &udp_port);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", &udp_port_async);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", &udp_port_result);
    implement("XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", &register_3);
    implement("XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged", &unregister);
    implement("XGameInviteRegisterForEvent", &register_3);
    implement("XGameInviteUnregisterForEvent", &unregister);
    implement("XGameInviteRegisterForPendingEvent", &register_3);
    implement("XGameInviteUnregisterForPendingEvent", &unregister);
    implement("XGameActivationRegisterForEvent", &register_3);
    implement("XGameActivationUnregisterForEvent", &unregister);
    implement("XGameProtocolRegisterForActivation", &register_3);
    implement("XGameProtocolUnregisterForActivation", &unregister);
    implement("XGameEventWrite", &succeeded);
    implement("XGameStreamingInitialize", &succeeded);
    implement("XGameStreamingUninitialize", &succeeded);
    implement("XGameStreamingIsStreaming", &no);
    implement("XAppBroadcastIsAppBroadcasting", &no);
}
} // namespace xs::runtime::modules::gameruntime
