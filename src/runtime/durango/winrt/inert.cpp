#include "runtime/durango/winrt/inert.hpp"
#include <cstdint>
#include <cwchar>

namespace xs::runtime::durango::winrt {
namespace {
struct Row {
    const wchar_t *type;
    GUID iid;
    std::uint8_t method;
    InertMethod behavior;
};
constexpr std::uint8_t digit(char c) {
    return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", lowercase, as the placeholder log prints it
constexpr GUID guid(const char (&text)[37]) {
    const auto byte = [&](int at) {
        return static_cast<std::uint8_t>(digit(text[at]) << 4 | digit(text[at + 1]));
    };
    const auto word = [&](int at) {
        return static_cast<std::uint16_t>(byte(at) << 8 | byte(at + 2));
    };
    return {static_cast<unsigned long>(word(0)) << 16 | word(4),
            word(9),
            word(14),
            {byte(19), byte(21), byte(24), byte(26), byte(28), byte(30), byte(32), byte(34)}};
}
constexpr auto game_transport_controls = guid("f5ba60d7-9303-44cd-ac0c-4e532702cd00");
constexpr auto current_app = guid("d52dc065-da3f-4685-995e-9b482eb5e603");
constexpr auto user_statics = guid("b4f23a93-7fc6-4a50-b153-950016389e50");
constexpr auto network_statics = guid("5074f851-950d-4165-9c15-365619481eea");
constexpr auto xbox_live_statics = guid("00193376-6a84-4c67-84a5-3b8a8aaea712");

constexpr wchar_t transport[] = L"Windows.Xbox.Media.GameTransportControls";
constexpr wchar_t store[] = L"Windows.ApplicationModel.Store.CurrentApp";
constexpr wchar_t license[] = L"Windows.ApplicationModel.Store.LicenseInformation";
constexpr wchar_t user[] = L"Windows.Xbox.System.User";

constexpr wchar_t network[] = L"Windows.Networking.Connectivity.NetworkInformation";

constexpr Row rows[] = {
    {transport, game_transport_controls, 14, {Inert::accept}}, // (bool)
    {transport, game_transport_controls, 16, {Inert::accept}}, // (bool), one per button
    {transport, game_transport_controls, 18, {Inert::accept}},
    {transport, game_transport_controls, 20, {Inert::accept}},
    {transport, game_transport_controls, 22, {Inert::accept}},
    {transport, game_transport_controls, 24, {Inert::accept}},
    {transport, game_transport_controls, 25, {Inert::event_add}}, // button pressed

    // public interfaces ICurrentApp and ILicenseInformation: a full license that never changes
    {store, current_app, 6, {Inert::object, license}},
    {license, direct, 7, {Inert::true8}},      // IsActive
    {license, direct, 8, {Inert::zero8}},      // IsTrial
    {license, direct, 10, {Inert::event_add}}, // LicenseChanged

    // one user, signed in from the start and for good
    {user, user_statics, 6, {Inert::shared_list, user}}, // Users
    {user, user_statics, 7, {Inert::event_add}},
    {user, user_statics, 10, {Inert::event_add}},
    {user, user_statics, 12, {Inert::event_add}},
    {user, user_statics, 26, {Inert::event_add}},

    // public interface INetworkInformationStatics
    {network, network_statics, 12, {Inert::event_add}}, // NetworkStatusChanged

    // event registrations for things that never happen here
    {L"Windows.Xbox.ApplicationModel.Store.Product",
     guid("76efb705-bdd4-4f6a-8d89-a9511d40afe7"),
     13,
     {Inert::event_add}}, // product purchased
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     7,
     {Inert::event_add}}, // package installed
    // console addition to CoreApplication
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("9ff3799e-6e83-468c-a5b0-501ce6bb0fa1"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("2fcec780-2e70-437a-ae46-72298ae2bac2"),
     7,
     {Inert::accept}}, // (bool)
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("6938fc66-ed0c-4b86-a240-009da675a0ec"),
     7,
     {Inert::accept}}, // (32-bit value)
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     11,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller", // Controllers: the gamepad is the one there is
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     6,
     {Inert::controllers}},
    {L"Windows.Xbox.Input.Controller", // ControllerAdded, ControllerRemoved
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     7,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     9,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     9,
     {Inert::shared_list, L"Windows.Xbox.System.User"}},
    {L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate",
     guid("46745a18-697a-47e1-bb62-cc71de0fdd8c"),
     6,
     {Inert::lookup, L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate"}},
    {L"Microsoft.Xbox.GameChat.ChatManager",
     guid("5ca654f2-7640-3bac-9c35-1a42a0e41f38"),
     6,
     {Inert::object, L"Microsoft.Xbox.GameChat.ChatManagerSettings"}},
    {L"Microsoft.Xbox.GameChat.ChatManagerSettings", direct, 23, {Inert::accept}},
    {L"Microsoft.Xbox.GameChat.ChatManagerSettings", direct, 25, {Inert::accept}},
    {L"Microsoft.Xbox.GameChat.ChatManagerSettings", direct, 27, {Inert::accept}},
    {L"Microsoft.Xbox.GameChat.ChatManagerSettings", direct, 29, {Inert::accept}},
    {L"Microsoft.Xbox.GameChat.ChatManagerSettings", direct, 31, {Inert::accept}},
    {L"Microsoft.Xbox.GameChat.ChatManager",
     guid("5ca654f2-7640-3bac-9c35-1a42a0e41f38"),
     7,
     {Inert::event_add}},
    {L"Microsoft.Xbox.GameChat.ChatManager",
     guid("5ca654f2-7640-3bac-9c35-1a42a0e41f38"),
     9,
     {Inert::event_add}},
    {L"Microsoft.Xbox.GameChat.ChatManager",
     guid("5ca654f2-7640-3bac-9c35-1a42a0e41f38"),
     11,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     18,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     20,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     22,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     6,
     {Inert::one64}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     8,
     {Inert::shared, L"Windows.Xbox.System.User"}},
    {L"Microsoft.Xbox.Services.XboxLiveContext",
     guid("bedc781e-a29e-3333-9d97-7af19812192d"),
     6,
     {Inert::lookup, L"Microsoft.Xbox.Services.XboxLiveContext"}},
    {L"Microsoft.Xbox.Services.Presence.PresenceData",
     guid("420b5889-6654-372f-9849-ddc556cd5864"),
     6,
     {Inert::lookup2, L"Microsoft.Xbox.Services.Presence.PresenceData"}},
    {L"Microsoft.Xbox.Services.XboxLiveContext",
     direct,
     16,
     {Inert::object, L"Microsoft.Xbox.Services.Presence.PresenceService"}},
    {L"Microsoft.Xbox.Services.Presence.PresenceService", direct, 7, {Inert::action3}},
    {L"Windows.Xbox.System.User", direct, 11, {Inert::true8}},
    {L"Windows.Xbox.System.User", direct, 10, {Inert::zero8}},
    {L"Windows.Xbox.System.User", direct, 15, {Inert::string, L"2814600000000001"}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     6,
     {Inert::empty_list2}},
    {L"Windows.Xbox.System.User",
     direct,
     9,
     {Inert::object, L"Windows.Xbox.System.UserDisplayInfo"}},
    {L"Windows.Xbox.System.UserDisplayInfo", direct, 6, {Inert::string, L"Player"}},
    {L"Windows.Xbox.System.UserDisplayInfo", direct, 9, {Inert::string, L"Player"}},
    {L"Microsoft.Xbox.Services.XboxLiveContext",
     direct,
     15,
     {Inert::object, L"Microsoft.Xbox.Services.Multiplayer.MultiplayerService"}},
    {L"Microsoft.Xbox.Services.Multiplayer.MultiplayerService",
     direct,
     13,
     {Inert::operation_at, L"Microsoft.Xbox.Services.Multiplayer.MultiplayerSession", 11}},
    {L"Microsoft.Xbox.Services.Multiplayer.MultiplayerSession", direct, 7, {Inert::zero32}},
    {L"Microsoft.Xbox.Services.XboxLiveContext",
     direct,
     21,
     {Inert::object, L"Microsoft.Xbox.Services.XboxLiveContext.Method21"}},
    {L"Microsoft.Xbox.Services.XboxLiveContext.Method21",
     direct,
     9,
     {Inert::operation_at, L"Microsoft.Xbox.Services.XboxLiveContext.Method21.Result", 4}},
    {L"Microsoft.Xbox.Services.XboxLiveContext.Method21.Result", direct, 7, {Inert::zero32}},
    {L"Microsoft.Xbox.Services.XboxLiveContext.Method21.Result", direct, 8, {Inert::zero8}},
    {L"Microsoft.Xbox.GameChat.ChatManager",
     guid("5ca654f2-7640-3bac-9c35-1a42a0e41f38"),
     23,
     {Inert::empty_list}},
    {L"Windows.Kinect.KinectSensor",
     guid("c421d46b-5084-45d6-bdf0-57151f81d0d9"),
     6,
     {Inert::zero64}},
    {L"Windows.Xbox.System.User", direct, 7, {Inert::empty_list}},
    // IXboxLiveConfigurationStatics: TitleId (decimal), PrimaryServiceConfigId, SandboxId
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     6,
     {Inert::title_string, L"title_id"}},
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     7,
     {Inert::title_string, L"service_config_id"}},
    {L"Windows.Xbox.Services.XboxLiveConfiguration",
     xbox_live_statics,
     8,
     {Inert::title_string, L"sandbox_id"}},
};
} // namespace

const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method) {
    for (const auto &row : rows)
        if (row.method == method && row.iid == iid && !std::wcscmp(row.type, type))
            return &row.behavior;
    return nullptr;
}
} // namespace xs::runtime::durango::winrt
