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

constexpr wchar_t transport[] = L"Windows.Xbox.Media.GameTransportControls";
constexpr wchar_t store[] = L"Windows.ApplicationModel.Store.CurrentApp";
constexpr wchar_t license[] = L"Windows.ApplicationModel.Store.LicenseInformation";
constexpr wchar_t user[] = L"Windows.Xbox.System.User";
constexpr wchar_t no_users[] = L"IVectorView<Windows.Xbox.System.User>";
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

    // nobody is signed in
    {user, user_statics, 6, {Inert::object, no_users}}, // Users
    {no_users, direct, 7, {Inert::zero32}},             // Size
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
    {L"IVectorView<Windows.Xbox.System.User>", direct, 9, {Inert::no_items}},
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     11,
     {Inert::event_add}},
    {L"Windows.Xbox.System.User",
     guid("b4f23a93-7fc6-4a50-b153-950016389e50"),
     9,
     {Inert::object, L"IVectorView<Windows.Xbox.System.User>"}},
    {L"Windows.Xbox.Storage.ConnectedStorageSpace",
     guid("07020414-b3e9-47c8-a36f-f24e2dc65573"),
     8,
     {Inert::operation, L"Windows.Xbox.Storage.ConnectedStorageSpace"}},
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
     {Inert::zero64}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     8,
     {Inert::zero64}},
};
} // namespace

const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method) {
    for (const auto &row : rows)
        if (row.method == method && row.iid == iid && !std::wcscmp(row.type, type))
            return &row.behavior;
    return nullptr;
}
} // namespace xs::runtime::durango::winrt
