#include "backends/audio/audio.hpp"
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

namespace xv::audio {
// Mutes the audio session of this process on the default output, which is where a title's
// sound ends up. The setting belongs to the session, so it can be made before any sound.
bool mute_process() {
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager> manager;
    ComPtr<ISimpleAudioVolume> volume;
    const bool muted =
        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void **>(manager.GetAddressOf()))) &&
        SUCCEEDED(manager->GetSimpleAudioVolume(nullptr, FALSE, &volume)) &&
        SUCCEEDED(volume->SetMute(TRUE, nullptr));
    volume.Reset();
    manager.Reset();
    device.Reset();
    enumerator.Reset();
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return muted;
}
} // namespace xv::audio
