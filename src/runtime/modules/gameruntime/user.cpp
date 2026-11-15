#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

// XUser: one user, signed in, who never signs out: the one of Options > Console. Its handle
// is the same object whoever asks, and nothing of it comes from Xbox Live.
namespace xs::runtime::modules::gameruntime {
namespace {
struct User {
} user;
constexpr std::uint64_t local_id = 1;
const char add_identity[] = "XUserAddAsync";
const char privilege_identity[] = "XUserResolvePrivilegeWithUiAsync";
const char token_identity[] = "XUserGetTokenAndSignatureAsync";
const char picture_identity[] = "XUserGetGamerPictureAsync";
const char issue_identity[] = "XUserResolveIssueWithUiAsync";
std::atomic<std::uint64_t> tokens{0};

std::uint64_t xbox_user_id() {
    return std::wcstoull(console_settings().xbox_user_id.c_str(), nullptr, 10);
}
std::string gamertag() {
    const auto &wide = console_settings().gamertag;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), size,
                        nullptr, nullptr);
    return text;
}
HRESULT hand_out(User **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &user;
    return S_OK;
}

HRESULT WINAPI duplicate_handle(void *, User *, User **duplicated) { return hand_out(duplicated); }
void WINAPI close_handle(void *, User *) {}
std::int32_t WINAPI compare(void *, User *first, User *second) {
    return first == second ? 0 : first < second ? -1 : 1;
}
HRESULT WINAPI get_max_users(void *, std::uint32_t *count) {
    if (!count)
        return E_POINTER;
    *count = 1;
    return S_OK;
}
HRESULT WINAPI add_async(void *, std::uint32_t, XAsyncBlock *block) {
    std::vector<std::uint8_t> payload(sizeof(User *));
    User *const handle = &user;
    std::memcpy(payload.data(), &handle, sizeof handle);
    return finish(block, add_identity, S_OK, std::move(payload));
}
HRESULT WINAPI add_result(void *, XAsyncBlock *block, User **handle) {
    return result(block, add_identity, sizeof *handle, handle, nullptr);
}
HRESULT WINAPI get_local_id(void *, User *, std::uint64_t *id) {
    if (!id)
        return E_POINTER;
    *id = local_id;
    return S_OK;
}
HRESULT WINAPI find_by_local_id(void *, std::uint64_t id, User **handle) {
    return id == local_id ? hand_out(handle) : E_INVALIDARG;
}
HRESULT WINAPI get_id(void *, User *, std::uint64_t *id) {
    if (!id)
        return E_POINTER;
    *id = xbox_user_id();
    return S_OK;
}
HRESULT WINAPI find_by_id(void *, std::uint64_t id, User **handle) {
    return id == xbox_user_id() ? hand_out(handle) : E_INVALIDARG;
}
HRESULT WINAPI get_is_guest(void *, User *, bool *guest) {
    if (!guest)
        return E_POINTER;
    *guest = false;
    return S_OK;
}
HRESULT WINAPI get_state(void *, User *, std::uint32_t *state) {
    if (!state)
        return E_POINTER;
    *state = 0; // signed in
    return S_OK;
}
// the first kits' form, without a component: the classic gamertag
HRESULT WINAPI get_gamertag_classic(void *, User *, std::size_t size, char *buffer,
                                    std::size_t *used) {
    return copy_text(gamertag().substr(0, 15), size, buffer, used);
}
// classic, modern, the modern one's suffix, modern with its suffix
HRESULT WINAPI get_gamertag(void *, User *, std::uint32_t component, std::size_t size,
                            char *buffer, std::size_t *used) {
    if (component > 3)
        return E_INVALIDARG;
    return copy_text(component == 2 ? std::string() : gamertag(), size, buffer, used);
}
HRESULT WINAPI get_age_group(void *, User *, std::uint32_t *group) {
    if (!group)
        return E_POINTER;
    *group = console_settings().age_group;
    return S_OK;
}
HRESULT WINAPI check_privilege(void *, User *, std::uint32_t, std::uint32_t, bool *has,
                               std::uint32_t *reason) {
    if (!has)
        return E_POINTER;
    *has = true;
    if (reason)
        *reason = 0;
    return S_OK;
}
HRESULT WINAPI resolve_privilege_async(void *, User *, std::uint32_t, std::uint32_t,
                                       XAsyncBlock *block) {
    return finish(block, privilege_identity, S_OK);
}
HRESULT WINAPI resolve_privilege_result(void *, XAsyncBlock *block) {
    return result(block, privilege_identity, 0, nullptr, nullptr);
}
// Xbox Live tokens: there is no service to give one
HRESULT WINAPI token_async(void *, User *, std::uint32_t, const void *, const void *, std::size_t,
                           const void *, std::size_t, const void *, XAsyncBlock *block) {
    return finish(block, token_identity, E_FAIL);
}
HRESULT WINAPI token_result_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
HRESULT WINAPI token_result(void *, XAsyncBlock *block, std::size_t size, void *buffer,
                            void **data, std::size_t *used) {
    if (data)
        *data = nullptr;
    return result(block, token_identity, size, buffer, used);
}
HRESULT WINAPI picture_async(void *, User *, std::uint32_t, XAsyncBlock *block) {
    return finish(block, picture_identity, E_FAIL);
}
HRESULT WINAPI picture_result(void *, XAsyncBlock *block, std::size_t size, void *buffer,
                              std::size_t *used) {
    return result(block, picture_identity, size, buffer, used);
}
HRESULT WINAPI resolve_issue_async(void *, User *, const void *, XAsyncBlock *block) {
    return finish(block, issue_identity, S_OK);
}
HRESULT WINAPI resolve_issue_result(void *, XAsyncBlock *block) {
    return result(block, issue_identity, 0, nullptr, nullptr);
}
// events of a user who never changes: a token, and no event
HRESULT WINAPI register_event(void *, Queue *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI unregister_event(void *, std::uint64_t, bool) { return true; }
HRESULT WINAPI sign_out_deferral(void *, void **deferral) {
    if (!deferral)
        return E_POINTER;
    *deferral = &user;
    return S_OK;
}
void WINAPI close_deferral(void *, void *) {}
// every input device is the one user's
HRESULT WINAPI find_for_device(void *, const void *, User **handle) { return hand_out(handle); }
bool WINAPI is_store_user(void *, User *) { return true; }
} // namespace

void install_user() {
    implement("XUserDuplicateHandle", &duplicate_handle);
    implement("XUserCloseHandle", &close_handle);
    implement("XUserCompare", &compare);
    implement("XUserGetMaxUsers", &get_max_users);
    implement("XUserAddAsync", &add_async);
    implement("XUserAddResult", &add_result);
    implement("XUserGetLocalId", &get_local_id);
    implement("XUserFindUserByLocalId", &find_by_local_id);
    implement("XUserGetId", &get_id);
    implement("XUserFindUserById", &find_by_id);
    implement("XUserGetIsGuest", &get_is_guest);
    implement("XUserGetState", &get_state);
    implement("XUserGetGamertagClassic", &get_gamertag_classic);
    implement("XUserGetGamertag", &get_gamertag);
    implement("XUserGetAgeGroup", &get_age_group);
    implement("XUserCheckPrivilege", &check_privilege);
    implement("XUserResolvePrivilegeWithUiAsync", &resolve_privilege_async);
    implement("XUserResolvePrivilegeWithUiResult", &resolve_privilege_result);
    implement("XUserGetTokenAndSignatureAsync", &token_async);
    implement("XUserGetTokenAndSignatureUtf16Async", &token_async);
    implement("XUserGetTokenAndSignatureResultSize", &token_result_size);
    implement("XUserGetTokenAndSignatureUtf16ResultSize", &token_result_size);
    implement("XUserGetTokenAndSignatureResult", &token_result);
    implement("XUserGetTokenAndSignatureUtf16Result", &token_result);
    implement("XUserGetGamerPictureAsync", &picture_async);
    implement("XUserGetGamerPictureResultSize", &token_result_size);
    implement("XUserGetGamerPictureResult", &picture_result);
    implement("XUserResolveIssueWithUiAsync", &resolve_issue_async);
    implement("XUserResolveIssueWithUiUtf16Async", &resolve_issue_async);
    implement("XUserResolveIssueWithUiResult", &resolve_issue_result);
    implement("XUserResolveIssueWithUiUtf16Result", &resolve_issue_result);
    implement("XUserRegisterForChangeEvent", &register_event);
    implement("XUserUnregisterForChangeEvent", &unregister_event);
    implement("XUserRegisterForDeviceAssociationChanged", &register_event);
    // the default audio endpoint never changes here
    implement("XUserRegisterForDefaultAudioEndpointUtf16Changed", &register_event);
    implement("XUserUnregisterForDefaultAudioEndpointUtf16Changed", &unregister_event);
    implement("XUserUnregisterForDeviceAssociationChanged", &unregister_event);
    implement("XUserGetSignOutDeferral", &sign_out_deferral);
    implement("XUserCloseSignOutDeferralHandle", &close_deferral);
    implement("XUserFindForDevice", &find_for_device);
    implement("XUserIsStoreUser", &is_store_user);
}
} // namespace xs::runtime::modules::gameruntime
