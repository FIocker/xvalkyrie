#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstring>

// What leaves a console as video: a stream to a remote client (XGameStreaming), a broadcast
// (XAppBroadcast) and clips or screenshots (XAppCapture). The title is played where it runs,
// so no client is ever connected, and the user's capture setting is "not by games".
// Signatures and structures are those of XGameStreaming.h and XAppCapture.h.
namespace xv::runtime::modules::gameruntime {
namespace {
constexpr HRESULT e_client_not_connected = static_cast<HRESULT>(0x89245401);
constexpr HRESULT e_no_datacenter = static_cast<HRESULT>(0x89245403);
constexpr HRESULT e_not_streaming_controller = static_cast<HRESULT>(0x89245404);
constexpr HRESULT e_invalid_handle = static_cast<HRESULT>(0x8924010C);
constexpr HRESULT e_not_found = static_cast<HRESULT>(0x80070490); // ERROR_NOT_FOUND
std::atomic<std::uint64_t> tokens{0};

using Client = std::uint64_t;
HRESULT WINAPI client_register(void *, Client, Queue *, void *, void *, std::uint64_t *token) {
    if (token)
        *token = 0;
    return e_client_not_connected;
}
bool WINAPI client_unregister(void *, Client, std::uint64_t, bool) { return false; }
HRESULT WINAPI connection_register(void *, Queue *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI connection_unregister(void *, std::uint64_t, bool) { return true; }
std::uint32_t WINAPI connection_state(void *, Client) { return 0; } // disconnected
HRESULT WINAPI physical_dimensions(void *, Client, std::uint32_t *horizontal,
                                   std::uint32_t *vertical) {
    if (horizontal)
        *horizontal = 0;
    if (vertical)
        *vertical = 0;
    return e_client_not_connected;
}
HRESULT WINAPI added_latency(void *, Client, std::uint32_t *input, std::uint32_t *output,
                             std::uint32_t *deviation) {
    for (auto *const value : {input, output, deviation})
        if (value)
            *value = 0;
    return e_client_not_connected;
}
std::size_t WINAPI no_size(void *) { return 0; }
HRESULT WINAPI server_location(void *, std::size_t size, char *name) {
    if (name && size)
        *name = '\0';
    return e_no_datacenter;
}
void WINAPI on_client(void *, Client) {}
void WINAPI layout_on_client(void *, Client, const char *) {}
HRESULT WINAPI touch_enabled(void *, Client, bool *enabled) {
    if (enabled)
        *enabled = false;
    return e_client_not_connected;
}
HRESULT WINAPI last_frame(void *, Client, std::uint64_t *frame) {
    if (frame)
        *frame = 0;
    return e_client_not_connected;
}
HRESULT WINAPI associated_frame(void *, void *, std::uint64_t *frame) {
    if (frame)
        *frame = 0;
    return e_not_streaming_controller;
}
// every button, stick and trigger of a reading is a real one: XGameStreamingGamepadPhysicality
// AllPhysical
HRESULT WINAPI gamepad_physicality(void *, void *reading, std::uint64_t *physicality) {
    if (!reading || !physicality)
        return E_POINTER;
    *physicality = 0x3FF3FF;
    return S_OK;
}
// touch layouts for all clients, of which there are none
HRESULT WINAPI update_state(void *, std::size_t, const void *) { return S_OK; }
HRESULT WINAPI show_with_state(void *, const char *, std::size_t, const void *) { return S_OK; }
HRESULT WINAPI update_state_on_client(void *, Client, std::size_t, const void *) {
    return e_client_not_connected;
}
HRESULT WINAPI show_with_state_on_client(void *, Client, const char *, std::size_t, const void *) {
    return e_client_not_connected;
}
std::size_t WINAPI bundle_name_size(void *, Client) { return 0; }
HRESULT WINAPI bundle_version(void *, Client, std::uint64_t *version, std::size_t size,
                              char *name) {
    if (version)
        *version = 0;
    if (name && size)
        *name = '\0';
    return e_client_not_connected;
}
HRESULT WINAPI client_address(void *, Client, std::size_t size, char *address) {
    if (address && size)
        *address = '\0';
    return e_client_not_connected;
}
HRESULT WINAPI session_id(void *, Client, std::size_t size, char *id, std::size_t *used) {
    if (id && size)
        *id = '\0';
    if (used)
        *used = 0;
    return e_client_not_connected;
}
// XGameStreamingDisplayDetails is 44 bytes: two sizes, a RECT, three limits and flags
HRESULT WINAPI display_details(void *, Client, std::uint32_t, float, float, void *details) {
    if (details)
        std::memset(details, 0, 44);
    return e_client_not_connected;
}
HRESULT WINAPI set_resolution(void *, std::uint32_t, std::uint32_t) {
    return e_client_not_connected;
}

struct BroadcastStatus {
    bool can_start, any_app_broadcasting, capture_resource_unavailable, game_stream_in_progress,
        gpu_constrained, app_inactive, blocked_for_app, disabled_by_user, disabled_by_system;
};
HRESULT WINAPI broadcast_status(void *, void *, BroadcastStatus *status) {
    if (!status)
        return E_POINTER;
    *status = {};
    status->disabled_by_system = true;
    return S_OK;
}
HRESULT WINAPI broadcast_ui(void *, void *) { return S_OK; }

struct VideoCaptureSettings {
    std::uint32_t width, height;
    std::uint64_t max_record_timespan_ms;
    std::uint8_t encoding, color_format; // H.264, SDR
    bool capture_by_games_allowed;
};
HRESULT WINAPI capture_settings(void *, VideoCaptureSettings *settings) {
    if (!settings)
        return E_POINTER;
    *settings = {1920, 1080, 0, 0, 0, false};
    return S_OK;
}
// XAppCaptureDiagnosticScreenshotResult starts with its count of files
HRESULT WINAPI diagnostic_screenshot(void *, bool, std::uint16_t, const char *,
                                     std::size_t *result) {
    if (result)
        *result = 0;
    return E_ACCESSDENIED;
}
HRESULT WINAPI diagnostic_clip(void *, std::int64_t, std::uint32_t, const char *, void *) {
    return E_ACCESSDENIED;
}
HRESULT WINAPI take_screenshot(void *, void *, void *) { return E_ACCESSDENIED; }
HRESULT WINAPI record_timespan(void *, const void *, std::uint64_t, void *) {
    return E_ACCESSDENIED;
}
HRESULT WINAPI start_user_record(void *, void *, std::uint32_t size, char *local_id) {
    if (local_id && size)
        *local_id = '\0';
    return E_ACCESSDENIED;
}
// nothing was captured, so no local id names anything and no stream is open
HRESULT WINAPI open_screenshot(void *, const char *, std::uint16_t, void **handle,
                               std::uint64_t *total) {
    if (handle)
        *handle = nullptr;
    if (total)
        *total = 0;
    return e_not_found;
}
HRESULT WINAPI stop_user_record(void *, const char *, void *) { return e_not_found; }
HRESULT WINAPI cancel_user_record(void *, const char *) { return e_not_found; }
HRESULT WINAPI read_stream(void *, void *, std::uint64_t, std::uint32_t, std::uint8_t *,
                           std::uint32_t *written) {
    if (written)
        *written = 0;
    return e_invalid_handle;
}
HRESULT WINAPI close_stream(void *, void *) { return e_invalid_handle; }
} // namespace

void install_streaming() {
    implement("XGameStreamingRegisterClientPropertiesChanged", &client_register);
    implement("XGameStreamingUnregisterClientPropertiesChanged", &client_unregister);
    implement("XGameStreamingGetStreamPhysicalDimensions", &physical_dimensions);
    implement("XGameStreamingGetConnectionState", &connection_state);
    implement("XGameStreamingRegisterConnectionStateChanged", &connection_register);
    implement("XGameStreamingUnregisterConnectionStateChanged", &connection_unregister);
    implement("XGameStreamingGetStreamAddedLatency", &added_latency);
    implement("XGameStreamingGetServerLocationNameSize", &no_size);
    implement("XGameStreamingGetServerLocationName", &server_location);
    implement("XGameStreamingHideTouchControlsOnClient", &on_client);
    implement("XGameStreamingShowTouchControlLayoutOnClient", &layout_on_client);
    implement("XGameStreamingIsTouchInputEnabled", &touch_enabled);
    implement("XGameStreamingGetLastFrameDisplayed", &last_frame);
    implement("XGameStreamingGetAssociatedFrame", &associated_frame);
    implement("XGameStreamingGetGamepadPhysicality", &gamepad_physicality);
    implement("XGameStreamingUpdateTouchControlsState", &update_state);
    implement("XGameStreamingUpdateTouchControlsStateOnClient", &update_state_on_client);
    implement("XGameStreamingShowTouchControlsWithStateUpdate", &show_with_state);
    implement("XGameStreamingShowTouchControlsWithStateUpdateOnClient", &show_with_state_on_client);
    implement("XGameStreamingGetTouchBundleVersionNameSize", &bundle_name_size);
    implement("XGameStreamingGetTouchBundleVersion", &bundle_version);
    implement("XGameStreamingGetClientIPAddress", &client_address);
    implement("XGameStreamingGetSessionId", &session_id);
    implement("XGameStreamingGetDisplayDetails", &display_details);
    implement("XGameStreamingSetResolution", &set_resolution);
    implement("XAppBroadcastShowUI", &broadcast_ui);
    implement("XAppBroadcastGetStatus", &broadcast_status);
    implement("XAppCaptureGetVideoCaptureSettings", &capture_settings);
    implement("XAppCaptureTakeDiagnosticScreenshot", &diagnostic_screenshot);
    implement("XAppCaptureRecordDiagnosticClip", &diagnostic_clip);
    implement("XAppCaptureTakeScreenshot", &take_screenshot);
    implement("XAppCaptureOpenScreenshotStream", &open_screenshot);
    implement("XAppCaptureReadScreenshotStream", &read_stream);
    implement("XAppCaptureCloseScreenshotStream", &close_stream);
    implement("XAppCaptureRecordTimespan", &record_timespan);
    implement("XAppCaptureReadLocalStream", &read_stream);
    implement("XAppCaptureCloseLocalStream", &close_stream);
    implement("XAppCaptureStartUserRecord", &start_user_record);
    implement("XAppCaptureStopUserRecord", &stop_user_record);
    implement("XAppCaptureCancelUserRecord", &cancel_user_record);
}
} // namespace xv::runtime::modules::gameruntime
