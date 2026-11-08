#include "runtime/modules/gameinput.hpp"
#include "backends/input/input.hpp"
#include "runtime/common/runtime.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <windows.h>

// GameInput.dll: how a GDK title reads its input. There is one device, a gamepad that is
// always connected, whose readings are what the emulated gamepad reads at the moment the
// title asks. The interfaces are COM ones with the methods in the order of the public
// GameInput header of the first kits.
namespace xs::runtime::modules {
namespace {
using Slot = void *;
constexpr std::uint32_t kind_gamepad = 0x00040000, kind_controller = 0x0000000e,
                        kind_ui_navigation = 0x01000000;
constexpr HRESULT reading_not_found = static_cast<HRESULT>(0x838A0003);
constexpr std::uint32_t device_connected = 1;

struct Object {
    Slot *vtable;
};
template <const char *Name, std::size_t Index>
HRESULT WINAPI unimplemented(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented GameInput call: %s slot %zu (%llx, %llx) from %s\n",
                     Name, Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <const char *Name, std::size_t... Index>
void fill(Slot *vtable, std::index_sequence<Index...>) {
    ((vtable[Index] = reinterpret_cast<Slot>(&unimplemented<Name, Index>)), ...);
}
HRESULT WINAPI query(Object *self, const GUID *, void **object) {
    if (!object)
        return E_POINTER;
    *object = self;
    return S_OK;
}
ULONG WINAPI reference(Object *) { return 1; }

constexpr char device_name[] = "IGameInputDevice", reading_name[] = "IGameInputReading",
               input_name[] = "IGameInput";
Slot device_vtable[24], reading_vtable[32], input_vtable[32];
Object device{device_vtable}, input{input_vtable};

// GameInputDeviceInfo: its size first; the rest says nothing about a device that has no
// hardware behind it
std::uint8_t device_info[1024];
const void *WINAPI get_device_info(Object *) {
    const std::uint32_t size = sizeof device_info;
    std::memcpy(device_info, &size, sizeof size);
    return device_info;
}
std::uint32_t WINAPI get_device_status(Object *) { return device_connected; }
void WINAPI set_rumble(Object *, const void *) {}

// One reading: the state at the moment it was asked for
struct Reading {
    Slot *vtable;
    std::atomic<ULONG> references;
    input::GamepadState state;
    std::uint64_t sequence, timestamp;
};
std::uint64_t now() {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(counter.QuadPart) * 1000000 /
           static_cast<std::uint64_t>(frequency.QuadPart);
}
ULONG WINAPI reading_add_ref(Reading *self) { return ++self->references; }
ULONG WINAPI reading_release(Reading *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
std::uint32_t WINAPI reading_kind(Reading *) { return kind_gamepad; }
std::uint64_t WINAPI reading_sequence(Reading *self, std::uint32_t) { return self->sequence; }
std::uint64_t WINAPI reading_timestamp(Reading *self) { return self->timestamp; }
void WINAPI reading_device(Reading *, Object **out) {
    if (out)
        *out = &device;
}
std::uint32_t WINAPI no_count(Reading *) { return 0; }
// GameInputGamepadState: the buttons two bits lower than the emulated gamepad has them
struct GamepadState {
    std::uint32_t buttons;
    float left_trigger, right_trigger, left_x, left_y, right_x, right_y;
};
bool WINAPI reading_gamepad(Reading *self, GamepadState *state) {
    if (!state)
        return false;
    const auto &read = self->state;
    *state = {read.buttons >> 2, read.left_trigger, read.right_trigger, read.left_x,
              read.left_y,       read.right_x,      read.right_y};
    return true;
}
bool WINAPI no_state(Reading *, void *) { return false; }

std::atomic<std::uint64_t> sequence{0};
HRESULT WINAPI current_reading(Object *, std::uint32_t kind, Object *wanted, Reading **reading) {
    if (!reading)
        return E_POINTER;
    *reading = nullptr;
    if (!(kind & (kind_gamepad | kind_controller)) || (wanted && wanted != &device))
        return reading_not_found;
    *reading = new Reading{reading_vtable, 1, input::poll(), ++sequence, now()};
    return S_OK;
}
// the readings before and after one are not kept
HRESULT WINAPI other_reading(Object *, Reading *, std::uint32_t, Object *, Reading **reading) {
    if (reading)
        *reading = nullptr;
    return reading_not_found;
}
std::uint64_t WINAPI current_timestamp(Object *) { return now(); }
std::atomic<std::uint64_t> tokens{0};
using DeviceCallback = void CALLBACK(std::uint64_t token, void *context, Object *device,
                                     std::uint64_t timestamp, std::uint32_t status,
                                     std::uint32_t previous);
// the one device is reported at once, and never again: it does not come or go
HRESULT WINAPI register_device_callback(Object *, Object *, std::uint32_t kind, std::uint32_t,
                                        std::uint32_t, void *context, DeviceCallback *callback,
                                        std::uint64_t *token) {
    const auto id = ++tokens;
    if (token)
        *token = id;
    if (callback && (kind & (kind_gamepad | kind_controller | kind_ui_navigation)))
        callback(id, context, &device, now(), device_connected, 0);
    return S_OK;
}
HRESULT WINAPI register_other_callback(Object *, Object *, void *, void *, std::uint64_t *token) {
    if (token)
        *token = ++tokens;
    return S_OK;
}
void WINAPI stop_callback(Object *, std::uint64_t) {}
bool WINAPI unregister_callback(Object *, std::uint64_t, std::uint64_t) { return true; }
void WINAPI set_focus_policy(Object *, std::uint32_t) {}

template <class F> Slot slot(F *function) { return reinterpret_cast<Slot>(function); }
void build() {
    fill<device_name>(device_vtable, std::make_index_sequence<24>{});
    fill<reading_name>(reading_vtable, std::make_index_sequence<32>{});
    fill<input_name>(input_vtable, std::make_index_sequence<32>{});
    for (auto *vtable : {device_vtable, input_vtable}) {
        vtable[0] = slot(&query);
        vtable[1] = vtable[2] = slot(&reference);
    }
    device_vtable[3] = slot(&get_device_info);
    device_vtable[4] = slot(&get_device_status);
    device_vtable[10] = slot(&set_rumble);
    reading_vtable[0] = slot(&query);
    reading_vtable[1] = slot(&reading_add_ref);
    reading_vtable[2] = slot(&reading_release);
    reading_vtable[3] = slot(&reading_kind);
    reading_vtable[4] = slot(&reading_sequence);
    reading_vtable[5] = slot(&reading_timestamp);
    reading_vtable[6] = slot(&reading_device);
    for (const std::size_t count : {8, 10, 12, 14, 17})
        reading_vtable[count] = slot(&no_count);
    for (const std::size_t state : {16, 19, 20, 21, 23, 24})
        reading_vtable[state] = slot(&no_state);
    reading_vtable[22] = slot(&reading_gamepad);
    input_vtable[3] = slot(&current_timestamp);
    input_vtable[4] = slot(&current_reading);
    input_vtable[5] = input_vtable[6] = slot(&other_reading);
    input_vtable[8] = slot(&register_other_callback);
    input_vtable[9] = slot(&register_device_callback);
    input_vtable[10] = input_vtable[11] = slot(&register_other_callback);
    input_vtable[12] = slot(&stop_callback);
    input_vtable[13] = slot(&unregister_callback);
    input_vtable[21] = slot(&set_focus_policy);
}
HRESULT WINAPI game_input_create(Object **out) {
    if (!out)
        return E_POINTER;
    static const bool built = (build(), true);
    (void)built;
    *out = &input;
    return S_OK;
}
} // namespace

void install_gameinput(Registry &registry) {
    registry.add("gameinput.dll", "GameInputCreate", reinterpret_cast<void *>(&game_input_create));
    registry.add("gameinput.dll", "#1", reinterpret_cast<void *>(&game_input_create));
}
} // namespace xs::runtime::modules
