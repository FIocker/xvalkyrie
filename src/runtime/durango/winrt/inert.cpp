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
constexpr wchar_t display_info[] = L"Windows.Xbox.System.UserDisplayInfo";
constexpr wchar_t audio_device[] = L"Windows.Xbox.System.AudioDeviceInfo";

constexpr wchar_t system_ui[] = L"Windows.Xbox.UI.SystemUI";
constexpr auto system_ui_statics = guid("1a0dd80e-3f9d-4cfa-8e50-0e2ebd1aa72f");
constexpr auto system_ui_statics7 = guid("01f75706-9df0-44d8-837c-a930da05a74a");

constexpr wchar_t party[] = L"Windows.Xbox.Multiplayer.Party";
constexpr wchar_t party_view[] = L"Windows.Xbox.Multiplayer.PartyView";
constexpr wchar_t party_chat[] = L"Windows.Xbox.Multiplayer.PartyChat";
constexpr wchar_t party_chat_view[] = L"Windows.Xbox.Multiplayer.PartyChatView";
constexpr auto party_statics = guid("e6110c4c-3660-4230-9880-208b149a3ca4");
constexpr auto party_statics2 = guid("87521843-2298-43c4-81f1-c7c50ab794e1");
constexpr auto party_statics3 = guid("15946429-17a1-4fdf-9d67-766ed0f4b868");
constexpr auto party_statics4 = guid("4b907f32-9165-4193-9cce-1d5ccddd060e");
constexpr auto party_chat_statics = guid("cc1bcf39-99f3-42a2-afb6-cdc07b29755f");

constexpr wchar_t association_template[] =
    L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate";
constexpr wchar_t association[] = L"Windows.Xbox.Networking.SecureDeviceAssociation";
constexpr wchar_t device_address[] = L"Windows.Xbox.Networking.SecureDeviceAddress";
constexpr wchar_t quality_of_service[] = L"Windows.Xbox.Networking.QualityOfService";
constexpr wchar_t network_cache[] = L"Windows.Xbox.Networking.NetworkCache";
constexpr wchar_t error_messages[] = L"Windows.Xbox.Networking.ErrorMessages";

constexpr wchar_t clip_capture[] = L"Windows.Xbox.Media.Capture.ApplicationClipCapture";
constexpr wchar_t clip_content[] = L"Windows.Xbox.Media.Capture.ClipContentAttributes";
constexpr wchar_t clip_info[] = L"Windows.Xbox.Media.Capture.ApplicationClipInfo";
constexpr wchar_t clip[] = L"Windows.Xbox.Media.Capture.ApplicationClip";
constexpr wchar_t clip_query[] = L"Windows.Xbox.Media.Capture.ApplicationClipQuery";
constexpr wchar_t clip_event[] = L"Windows.Xbox.Media.Capture.ApplicationClipEventData";
constexpr wchar_t clip_metadata[] = L"Windows.Xbox.Media.Capture.ApplicationClipUpdateMetadataInfo";
constexpr wchar_t diagnostic_capture[] = L"Windows.Xbox.Media.Capture.DiagnosticCapture";
// ApplicationClipError::ServiceNotRunning: game DVR is not there
constexpr long no_recording = -2110640127;
// HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE)
constexpr long network_unreachable = static_cast<long>(0x800704CF);

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
    // public ICoreAcceleratorKeys: AcceleratorKeyActivated
    {L"Windows.UI.Core.CoreDispatcher",
     guid("9ffdf7f5-b8c9-4ef0-b7d2-1de626561fc8"),
     6,
     {Inert::event_add}},
    {L"Windows.UI.Core.CoreDispatcher",
     guid("9ffdf7f5-b8c9-4ef0-b7d2-1de626561fc8"),
     7,
     {Inert::accept}},
    // public IMouseDeviceStatics::GetForCurrentView and IMouseDevice::MouseMoved
    {L"Windows.Devices.Input.MouseDevice",
     guid("f5a82ce3-4a2d-4004-b386-c74dc69be979"),
     6,
     {Inert::shared, L"Windows.Devices.Input.MouseDevice"}},
    {L"Windows.Devices.Input.MouseDevice", direct, 6, {Inert::event_add}},
    {L"Windows.Devices.Input.MouseDevice", direct, 7, {Inert::accept}},
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
    {L"Windows.Xbox.System.User", direct, 15, {Inert::console_string, L"xbox_user_id"}},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager",
     guid("971feaaf-98f8-4038-b8c1-3a5f1a043b97"),
     6,
     {Inert::empty_list2}},
    // the display info of the one user, the same object every time it is asked for
    {L"Windows.Xbox.System.User",
     direct,
     9,
     {Inert::shared, L"Windows.Xbox.System.UserDisplayInfo"}},
    // IUserDisplayInfo: Gamertag, GamerScore, ApplicationDisplayName, GameDisplayName,
    // Reputation, AgeGroup, all from Options > Console
    {display_info, direct, 6, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 7, {Inert::console32, L"gamerscore"}},
    {display_info, direct, 8, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 9, {Inert::console_string, L"gamertag"}},
    {display_info, direct, 10, {Inert::zero32}},
    {display_info, direct, 11, {Inert::console32, L"age_group"}},
    {L"Windows.Kinect.KinectSensor",
     guid("c421d46b-5084-45d6-bdf0-57151f81d0d9"),
     6,
     {Inert::zero64}},
    {L"Windows.Xbox.System.User", direct, 7, {Inert::audio_devices}},
    // IAudioDeviceInfo of the headset: the host's communications output, shared, with no
    // microphone to unmute
    {audio_device, direct, 6, {Inert::console_string, L"audio_device_id"}},
    {audio_device, direct, 7, {Inert::number32, nullptr, 1}}, // DeviceType: render
    {audio_device, direct, 8, {Inert::number32, nullptr, 2}}, // Sharing: shared
    {audio_device, direct, 9, {Inert::zero32}},               // Category: communications
    {audio_device, direct, 10, {Inert::true8}},               // IsMicrophoneMuted
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

    // ISystemUIStatics: the guide, profile cards, invites, party and achievements are not
    // there and close at once; the keyboard is a window of the host (system_ui.cpp)
    {system_ui, system_ui_statics, 6, {Inert::action_at, nullptr, 3}},  // ShowEpgAsync
    {system_ui, system_ui_statics, 7, {Inert::action_at, nullptr, 2}},  // ShowMiniguideAsync
    {system_ui, system_ui_statics, 8, {Inert::action_at, nullptr, 3}},  // ShowProfileCardAsync
    {system_ui, system_ui_statics, 9, {Inert::action_at, nullptr, 2}},  // ShowSendInvitesAsync
    {system_ui, system_ui_statics, 11, {Inert::keyboard}},              // ShowVirtualKeyboardAsync
    {system_ui, system_ui_statics, 12, {Inert::action_at, nullptr, 2}}, // LaunchPartyAsync
    {system_ui, system_ui_statics, 13, {Inert::action_at, nullptr, 3}}, // LaunchAchievementsAsync
    {system_ui, system_ui_statics7, 6, {Inert::keyboard_options}},
    {system_ui, system_ui_statics7, 7, {Inert::action_at, nullptr, 4}}, // ShowSendGameInvites
    {system_ui, system_ui_statics7, 8, {Inert::action_at, nullptr, 5}},
    {system_ui, guid("197625d0-af45-4f9c-b068-d5bda0e60958"), 6, {Inert::accept}},

    // IPartyStatics..4: no party. Its view is empty, registering sessions succeeds and the
    // events are never raised.
    {party, party_statics, 6, {Inert::action_at, nullptr, 3}},          // RegisterGameSessionAsync
    {party, party_statics, 7, {Inert::action_at, nullptr, 3}},          // RegisterMatchSessionAsync
    {party, party_statics, 8, {Inert::action_at, nullptr, 3}},          // InviteToPartyAsync
    {party, party_statics, 9, {Inert::operation, party_view}},          // GetPartyViewAsync
    {party, party_statics, 10, {Inert::action_at, nullptr, 3}},         // AddLocalUsersAsync
    {party, party_statics, 11, {Inert::action_at, nullptr, 3}},         // PullInactivePlayersAsync
    {party, party_statics, 12, {Inert::action_at, nullptr, 3}},         // PullReservedPlayersAsync
    {party, party_statics, 13, {Inert::list_operation_at, nullptr, 2}}, // GetAvailableGamePlayers
    {party, party_statics, 14, {Inert::list_operation_at, nullptr, 2}}, // ...GroupedByDevice
    {party, party_statics, 15, {Inert::action_at, nullptr, 2}},         // SwitchPartyTitleAsync
    {party, party_statics, 16, {Inert::event_add}},                     // GameSessionReady
    {party, party_statics, 17, {Inert::accept}},
    {party, party_statics, 18, {Inert::event_add}}, // PartyStateChanged
    {party, party_statics, 19, {Inert::accept}},
    {party, party_statics, 20, {Inert::event_add}}, // PartyRosterChanged
    {party, party_statics, 21, {Inert::accept}},
    {party, party_statics, 22, {Inert::event_add}}, // MatchStatusChanged
    {party, party_statics, 23, {Inert::accept}},
    {party, party_statics, 24, {Inert::event_add}}, // GamePlayersChanged
    {party, party_statics, 25, {Inert::accept}},
    {party, party_statics2, 6, {Inert::operation_at, party_view, 2}}, // GetPartyViewAsyncEx
    {party, party_statics3, 6, {Inert::action_at, nullptr, 2}},       // RemoveLocalUsersAsync
    {party, party_statics3, 7, {Inert::zero64}},                      // PartyId: none
    {party, party_statics3, 8, {Inert::action_at, nullptr, 3}}, // DisassociateGameSessionAsync
    {party, party_statics4, 6, {Inert::action_at, nullptr, 4}}, // RegisterGameSessionCompare
    {party, party_statics4, 7, {Inert::action_at, nullptr, 4}}, // RegisterMatchSessionCompare
    {party, party_statics4, 8, {Inert::list_operation_at, nullptr, 3}}, // GetUserPartyAssociations
    {party, party_statics4, 9, {Inert::operation_at, party_view, 3}},   // GetPartyViewByPartyId
    {party, party_statics4, 10, {Inert::action_at, nullptr, 3}},        // JoinPartyByIdAsync
    {party, party_statics4, 11, {Inert::zero32}},                       // Joinability
    {party, party_statics4, 12, {Inert::accept}},
    // IPartyView: no members, no sessions, not in another title
    {party_view, direct, 6, {Inert::empty_list}},
    {party_view, direct, 7, {Inert::empty_list}},
    {party_view, direct, 8, {Inert::empty_list}},
    {party_view, direct, 9, {Inert::zero64}},
    {party_view, direct, 10, {Inert::zero64}},
    {party_view, direct, 11, {Inert::zero8}},
    // IPartyChatStatics: party chat is off and stays so
    {party_chat, party_chat_statics, 6, {Inert::operation, party_chat_view}},
    {party_chat, party_chat_statics, 7, {Inert::event_add}},
    {party_chat, party_chat_statics, 8, {Inert::accept}},
    {party_chat, party_chat_statics, 9, {Inert::zero8}},  // IsPartyChatActive
    {party_chat, party_chat_statics, 10, {Inert::zero8}}, // IsPartyChatSuppressed
    {party_chat, party_chat_statics, 11, {Inert::accept}},
    {party_chat, party_chat_statics, 12, {Inert::event_add}},
    {party_chat, party_chat_statics, 13, {Inert::accept}},
    {party_chat, party_chat_statics, 14, {Inert::event_add}},
    {party_chat, party_chat_statics, 15, {Inert::accept}},
    {party_chat_view, direct, 6, {Inert::empty_list}}, // Members
    // PartyConfig getters, next to the setters above
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("2fcec780-2e70-437a-ae46-72298ae2bac2"),
     6,
     {Inert::zero8}},
    {L"Windows.Xbox.Multiplayer.PartyConfig",
     guid("6938fc66-ed0c-4b86-a240-009da675a0ec"),
     6,
     {Inert::zero32}},

    // Secure device associations without a network: templates exist, creating an
    // association fails as unreachable, there is no association to look up, and the local
    // address is one object with strict NAT
    {association_template, direct, 6, {Inert::event_add}}, // AssociationIncoming
    {association_template, direct, 7, {Inert::accept}},
    {association_template, direct, 8, {Inert::offline, nullptr, 3}},  // CreateAssociationAsync
    {association_template, direct, 9, {Inert::offline, nullptr, 5}},  // ...ForPortsAsync
    {association_template, direct, 10, {Inert::offline, nullptr, 2}}, // CreateCertificateRequest
    {association_template, direct, 11, {Inert::failed_action_at, nullptr, 2, network_unreachable}},
    {association_template, direct, 20, {Inert::empty_list}}, // Associations
    {association,
     guid("15661441-f13a-46e4-8ac2-f474d59b1f0c"),
     6,
     {Inert::null_at, nullptr, 5}}, // GetAssociationBySocketAddressBytes
    {association,
     guid("15661441-f13a-46e4-8ac2-f474d59b1f0c"),
     7,
     {Inert::null_at, nullptr, 5}}, // GetAssociationByHostNamesAndPorts
    {device_address,
     guid("02addb0f-4880-4b1e-98b8-ee638ad89b55"),
     9,
     {Inert::shared, device_address}},               // GetLocal
    {device_address, direct, 6, {Inert::event_add}}, // BufferChanged
    {device_address, direct, 7, {Inert::accept}},
    {device_address, direct, 10, {Inert::zero64}},               // GetBase64String: empty
    {device_address, direct, 11, {Inert::true8}},                // IsLocal
    {device_address, direct, 12, {Inert::number32, nullptr, 2}}, // NetworkAccessType: strict
    {quality_of_service,
     guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"),
     6,
     {Inert::offline, nullptr, 5}}, // MeasureQualityOfServiceAsync
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 7, {Inert::accept}},
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 8, {Inert::accept}},
    {quality_of_service, guid("d7d51fb8-4e8e-4e11-b2a4-930f9c344a6a"), 9, {Inert::accept}},
    {network_cache, guid("bfff4b75-05a4-49b0-9383-c29df62a8da2"), 6, {Inert::accept}},
    {network_cache, guid("bfff4b75-05a4-49b0-9383-c29df62a8da2"), 7, {Inert::accept}},
    // GetMessageForError(code, HSTRING *): no message, the title shows its own
    {error_messages, guid("0759380d-a6d3-443a-9441-e45edc7677e6"), 6, {Inert::null_at, nullptr, 2}},

    // Game DVR is not running: recording fails as the console says when it is off, there
    // are no clips, and the metadata a title sets is accepted and forgotten
    {clip_capture, guid("82845b56-3150-4058-947a-b567537de0c2"), 6, {Inert::action}},
    {clip_capture, guid("82845b56-3150-4058-947a-b567537de0c2"), 7, {Inert::action}},
    {clip_capture, guid("2ac83090-107a-4cd7-8376-5188d3fcfa4e"), 6, {Inert::shared, clip_content}},
    {clip_capture, guid("2ac83090-107a-4cd7-8376-5188d3fcfa4e"), 7, {Inert::accept}},
    {clip_content, direct, 6, {Inert::zero32}},
    {clip_content, direct, 7, {Inert::accept}},
    {clip_capture, direct, 6, {Inert::failed_action_at, nullptr, 2, no_recording}},
    {clip_capture, direct, 7, {Inert::failed_progress_at, nullptr, 2, no_recording}},
    {clip_capture, direct, 8, {Inert::failed_progress_at, nullptr, 5, no_recording}},
    {clip_capture, direct, 9, {Inert::accept}},     // CancelRecord
    {clip_capture, direct, 10, {Inert::event_add}}, // Failed
    {clip_capture, direct, 11, {Inert::accept}},
    {clip_capture, direct, 12, {Inert::event_add}}, // RecordLimitationExceeded
    {clip_capture, direct, 13, {Inert::accept}},
    {clip_info, guid("509ccd19-fee4-45ab-8daa-7895328d452f"), 6, {Inert::lookup, clip_info}},
    {clip_info, direct, 6, {Inert::zero64}},
    {clip_info, direct, 7, {Inert::accept}},
    {clip_info, direct, 8, {Inert::empty_list}},
    {clip_info, direct, 9, {Inert::accept}},
    {clip_info, direct, 10, {Inert::zero64}},
    {clip_info, direct, 11, {Inert::accept}},
    {clip_info, direct, 12, {Inert::zero64}},
    {clip_info, direct, 13, {Inert::accept}},
    {clip_info, direct, 14, {Inert::zero64}},
    {clip_info, direct, 15, {Inert::accept}},
    {clip_info, direct, 16, {Inert::zero64}},
    {clip_info, direct, 17, {Inert::accept}},
    {clip_info, direct, 18, {Inert::zero64}},
    {clip_info, direct, 19, {Inert::accept}},
    {clip,
     guid("c0c846ba-0d69-4b4b-ac8b-dba211322f1d"),
     6,
     {Inert::failed_action_at, nullptr, 6, no_recording}}, // UpdateClipMetadataAsync
    {clip,
     guid("c0c846ba-0d69-4b4b-ac8b-dba211322f1d"),
     7,
     {Inert::failed_action_at, nullptr, 5, no_recording}},           // DeleteClipAsync
    {clip_query, direct, 6, {Inert::list_operation_at, nullptr, 2}}, // GetClipsAsync
    {clip_query, direct, 7, {Inert::list_operation_at, nullptr, 3}},
    {clip_query, direct, 8, {Inert::list_operation_at, nullptr, 1}}, // GetNextClipsAsync
    {clip_query,
     guid("c2d95416-127e-4342-b433-d95e7dc31baa"),
     6,
     {Inert::list_operation_at, nullptr, 3}},
    {clip_metadata, direct, 6, {Inert::zero64}},
    {clip_metadata, direct, 7, {Inert::accept}},
    {clip_metadata, direct, 8, {Inert::zero64}},
    {clip_metadata, direct, 9, {Inert::accept}},
    {clip_metadata, direct, 10, {Inert::zero32}},
    // IApplicationClipEventData: Int32 SectionId, String PlayerSessionId, then four Int32
    // and three Single, each a getter and a setter
    {clip_event, direct, 6, {Inert::zero32}},
    {clip_event, direct, 7, {Inert::accept}},
    {clip_event, direct, 8, {Inert::zero64}},
    {clip_event, direct, 9, {Inert::accept}},
    {clip_event, direct, 10, {Inert::zero32}},
    {clip_event, direct, 11, {Inert::accept}},
    {clip_event, direct, 12, {Inert::zero32}},
    {clip_event, direct, 13, {Inert::accept}},
    {clip_event, direct, 14, {Inert::zero32}},
    {clip_event, direct, 15, {Inert::accept}},
    {clip_event, direct, 16, {Inert::zero32}},
    {clip_event, direct, 17, {Inert::accept}},
    {clip_event, direct, 18, {Inert::zero32}},
    {clip_event, direct, 19, {Inert::accept}},
    {clip_event, direct, 20, {Inert::zero32}},
    {clip_event, direct, 21, {Inert::accept}},
    {clip_event, direct, 22, {Inert::zero32}},
    {clip_event, direct, 23, {Inert::accept}},
    {diagnostic_capture,
     guid("39f20721-379c-4255-b558-c133e3359857"),
     6,
     {Inert::failed_action_at, nullptr, 3, no_recording}}, // RecordTemporaryClipAsync

    // IInstalledSpeechRecognizersStatic: no microphone, so no recognizer
    {L"Windows.Xbox.Speech.Recognition.InstalledSpeechRecognizers",
     guid("606a630f-c5c3-4dfa-9950-d98bfe772c8f"),
     6,
     {Inert::empty_list}},
    {L"Windows.Xbox.Speech.Recognition.InstalledSpeechRecognizers",
     guid("606a630f-c5c3-4dfa-9950-d98bfe772c8f"),
     7,
     {Inert::zero64}},

    // IControllerStatics: ControllerOrderChanged; IController::Type of the one gamepad
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     13,
     {Inert::event_add}},
    {L"Windows.Xbox.Input.Controller",
     guid("7400624f-144b-47fc-9f3b-1dcafb482873"),
     14,
     {Inert::accept}},
    {L"Windows.Xbox.Input.Gamepad",
     guid("9776ff56-9bfd-4581-ad45-b645bba526d6"),
     7,
     {Inert::string, L"Windows.Xbox.Input.Gamepad"}},
};
} // namespace

const InertMethod *inert_method(const wchar_t *type, const GUID &iid, std::size_t method) {
    for (const auto &row : rows)
        if (row.method == method && row.iid == iid && !std::wcscmp(row.type, type))
            return &row.behavior;
    return nullptr;
}
} // namespace xs::runtime::durango::winrt
