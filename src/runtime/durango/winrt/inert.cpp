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
constexpr wchar_t achievements[] = L"Windows.Xbox.Achievements.AchievementNotifier";
constexpr wchar_t achievement_source[] = L"Windows.Xbox.Achievements.AchievementSource";
constexpr auto achievement_notifier_statics = guid("82ca180b-30e2-4ea0-a62e-b61697a2f334");
constexpr auto achievement_source_interface = guid("3b7e45dc-086e-4535-9a5d-c6e42cdea33c");
constexpr wchar_t chat_session[] = L"Windows.Xbox.Chat.ChatSession";
constexpr wchar_t chat_session_state[] = L"Windows.Xbox.Chat.ChatSessionState";
constexpr auto chat_session2 = guid("8840b65e-eb13-4629-8f63-2854624460ff");
constexpr wchar_t product[] = L"Windows.Xbox.ApplicationModel.Store.Product";
constexpr auto product_statics = guid("76efb705-bdd4-4f6a-8d89-a9511d40afe7");

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

    // IAchievementNotifierStatics: GetTitleIdFilteredSource, GetUnfilteredSource. The
    // AchievementUnlocked event of IAchievementSource is never raised: nothing unlocks here.
    {achievements, achievement_notifier_statics, 6, {Inert::object, achievement_source}},
    {achievements, achievement_notifier_statics, 7, {Inert::object, achievement_source}},
    {achievement_source, direct, 6, {Inert::event_add}},
    {achievement_source, direct, 7, {Inert::accept}},
    {achievement_source, achievement_source_interface, 6, {Inert::event_add}},
    {achievement_source, achievement_source_interface, 7, {Inert::accept}},

    // Chat with no microphone and no headset anywhere: sessions hold channels, their state
    // has no capture source and no render target, and nothing changes.
    // IChatSessionFactory: (TimeSpan, features, out) and (TimeSpan, affinity, features, out)
    {chat_session,
     guid("53cb6065-c211-4b21-a3ef-cd544d22aba1"),
     6,
     {Inert::object_at, chat_session, 3}},
    {chat_session,
     guid("53cb6065-c211-4b21-a3ef-cd544d22aba1"),
     7,
     {Inert::object_at, chat_session, 4}},
    {chat_session, direct, 6, {Inert::vector}},    // Channels
    {chat_session, direct, 7, {Inert::event_add}}, // StateChangedEvent
    {chat_session, direct, 8, {Inert::accept}},
    {chat_session, direct, 9, {Inert::operation, chat_session_state}}, // GetStateAsync
    {chat_session_state, direct, 6, {Inert::empty_list}},              // CaptureSources
    {chat_session_state, direct, 7, {Inert::empty_list}},              // RenderTargets
    // IChatSession2: TranscriptionEnabled, TranscribedTextReadyEvent
    {chat_session, chat_session2, 6, {Inert::zero32}},
    {chat_session, chat_session2, 7, {Inert::accept}},
    {chat_session, chat_session2, 9, {Inert::event_add}},
    {chat_session, chat_session2, 10, {Inert::accept}},

    // IProductStatics: the store dialogs close at once, every privilege is there, and
    // nothing is ever purchased
    {product, product_statics, 6, {Inert::action_at, nullptr, 3}},          // ShowPurchaseAsync
    {product, product_statics, 7, {Inert::action_at, nullptr, 3}},          // ShowDetailsAsync
    {product, product_statics, 8, {Inert::action_at, nullptr, 5}},          // ShowMarketplaceAsync
    {product, product_statics, 9, {Inert::action_at, nullptr, 3}},          // ShowRedeemCodeAsync
    {product, product_statics, 10, {Inert::action_at, nullptr, 4}},         // ShowSubscriptionAsync
    {product, product_statics, 11, {Inert::zero_operation_at, nullptr, 5}}, // CheckPrivilegeAsync
    {product, product_statics, 12, {Inert::zero_operation_at, nullptr, 5}}, // CheckPrivilegesAsync
    {product, product_statics, 13, {Inert::event_add}},                     // ProductPurchased
    {product, product_statics, 14, {Inert::accept}},

    // event registrations for things that never happen here
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
    {L"Windows.Xbox.System.User", direct, 11, {Inert::true8}},
    {L"Windows.Xbox.System.User", direct, 10, {Inert::zero8}},
    {L"Windows.Xbox.System.User", direct, 15, {Inert::string, L"2814600000000001"}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     6,
     {Inert::empty_list2}},
    // the display info of the one user, the same object every time it is asked for
    {L"Windows.Xbox.System.User",
     direct,
     9,
     {Inert::shared, L"Windows.Xbox.System.UserDisplayInfo"}},
    {L"Windows.Xbox.System.UserDisplayInfo", direct, 6, {Inert::string, L"Player"}},
    {L"Windows.Xbox.System.UserDisplayInfo", direct, 9, {Inert::string, L"Player"}},
    {L"Windows.Kinect.KinectSensor",
     guid("c421d46b-5084-45d6-bdf0-57151f81d0d9"),
     6,
     {Inert::zero64}},
    {L"Windows.Xbox.System.User", direct, 7, {Inert::empty_list}},
    // ShowAccountPickerAsync(controller, options, IAsyncOperation<AccountPickerResult> **)
    {L"Windows.Xbox.UI.SystemUI",
     guid("1a0dd80e-3f9d-4cfa-8e50-0e2ebd1aa72f"),
     10,
     {Inert::operation_at, L"Windows.Xbox.UI.AccountPickerResult", 3}},
    {L"Windows.Xbox.UI.AccountPickerResult", direct, 6, {Inert::shared, user}}, // User
    {user, direct, 6, {Inert::one32}},                                          // Id
    {user, direct, 8, {Inert::controllers}},                                    // Controllers
    {user, direct, 12, {Inert::zero32}},                                        // Location
    {user, direct, 13, {Inert::zero64}},                                        // Sponsor: none
    {user, direct, 14, {Inert::string, L"0000000000000001"}},                   // XboxUserHash
    // GetTokenAndSignatureAsync, three overloads: there is no network to sign with
    {user, direct, 16, {Inert::offline, nullptr, 4}},
    {user, direct, 17, {Inert::offline, nullptr, 6}},
    {user, direct, 18, {Inert::offline, nullptr, 5}},
    // a getter of an object the title does without when there is none
    {L"Windows.ApplicationModel.Core.CoreApplication",
     guid("1ada0e3e-e4a2-4123-b451-dc96bf800419"),
     6,
     {Inert::zero64}},
    // IPackageStatics::Current: the one package there is
    {L"Windows.ApplicationModel.Package",
     guid("4e534bdf-2960-4878-97a4-9624deb72f2d"),
     6,
     {Inert::shared, L"Windows.ApplicationModel.Package"}},
    // PackageTransferWatcher: the whole package is on disk, nothing is being transferred
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("a306e506-12f9-460a-b0ae-50ba328c786c"),
     6,
     {Inert::lookup2, L"Windows.Xbox.Management.Deployment.PackageTransferWatcher"}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("a306e506-12f9-460a-b0ae-50ba328c786c"),
     7,
     {Inert::lookup, L"Windows.Xbox.Management.Deployment.PackageTransferWatcher"}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 6, {Inert::empty_list}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 7, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 8, {Inert::accept}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 9, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 10, {Inert::accept}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 11, {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher", direct, 12, {Inert::accept}},
    // IPackageTransferWatcherTransferInfo: TransferStatus, TransferType, the status event
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     6,
     {Inert::zero32}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     7,
     {Inert::zero32}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     8,
     {Inert::event_add}},
    {L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",
     guid("ba1bad13-11e1-43ce-be44-845759796a7f"),
     9,
     {Inert::accept}},
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
