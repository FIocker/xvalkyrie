#include "runtime/common/com_forward.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#include <atomic>
#include <audioclient.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <string>
#include <string_view>
#include <thread>
#include <unknwn.h>
#include <windows.h>

// Console-only functions. Undocumented ones only return.
namespace xv::runtime::durango {
namespace {
class Activation final : public IActivateAudioInterfaceAsyncOperation {
  public:
    Activation(HRESULT result, IUnknown *audio) : result_(result), audio_(audio) {}
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IActivateAudioInterfaceAsyncOperation) &&
            iid != __uuidof(IAgileObject))
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left) {
            if (audio_)
                audio_->Release();
            delete this;
        }
        return left;
    }
    void finish(HRESULT result, IUnknown *audio) {
        result_ = result;
        audio_ = audio;
    }
    STDMETHODIMP GetActivateResult(HRESULT *result, IUnknown **audio) override {
        *result = result_;
        *audio = audio_;
        if (audio_)
            audio_->AddRef();
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    HRESULT result_;
    IUnknown *audio_;
};
// The console mixes any PCM format; the host does in shared mode when asked to convert
HRESULT STDMETHODCALLTYPE initialize(Forwarder *self, AUDCLNT_SHAREMODE mode, DWORD flags,
                                     REFERENCE_TIME duration, REFERENCE_TIME period,
                                     const WAVEFORMATEX *format, const GUID *session) {
    auto *const client = static_cast<IAudioClient *>(forwarder_host(self));
    if (mode == AUDCLNT_SHAREMODE_SHARED)
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const auto result = client->Initialize(mode, flags, duration, period, format, session);
    std::fprintf(stderr,
                 "[Runtime] audio client: %u channels, %lu Hz, %u bits, flags 0x%lx: 0x%08lx\n",
                 format ? format->nChannels : 0, format ? format->nSamplesPerSec : 0,
                 format ? format->wBitsPerSample : 0, flags, static_cast<unsigned long>(result));
    return result;
}
HRESULT STDMETHODCALLTYPE format_supported(Forwarder *, AUDCLNT_SHAREMODE mode,
                                           const WAVEFORMATEX *, WAVEFORMATEX **closest) {
    if (closest)
        *closest = nullptr;
    return mode == AUDCLNT_SHAREMODE_SHARED ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
}
// the console always mixes 7.1 in float; titles check for eight channels
HRESULT STDMETHODCALLTYPE mix_format(Forwarder *self, WAVEFORMATEX **format) {
    if (!format)
        return E_POINTER;
    WAVEFORMATEX *host = nullptr;
    const auto result = static_cast<IAudioClient *>(forwarder_host(self))->GetMixFormat(&host);
    if (FAILED(result))
        return result;
    auto *const mix =
        static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    *mix = {};
    mix->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    mix->Format.nChannels = 8;
    mix->Format.nSamplesPerSec = host->nSamplesPerSec;
    mix->Format.wBitsPerSample = 32;
    mix->Format.nBlockAlign = 8 * 4;
    mix->Format.nAvgBytesPerSec = mix->Format.nSamplesPerSec * mix->Format.nBlockAlign;
    mix->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    mix->Samples.wValidBitsPerSample = 32;
    mix->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    mix->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    CoTaskMemFree(host);
    *format = &mix->Format;
    return S_OK;
}
// the console's AudioClientProperties are not the desktop's
HRESULT STDMETHODCALLTYPE client_properties(Forwarder *, const void *) { return S_OK; }
HRESULT audio_client_query(Forwarder *self, REFIID iid, void **object) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClient) ||
        iid == __uuidof(IAudioClient2) || iid == __uuidof(IAudioClient3)) {
        forwarder_add_ref(self);
        *object = self;
        return S_OK;
    }
    return forwarder_host(self)->QueryInterface(iid, object);
}
IUnknown *audio_client(IUnknown *host) {
    IAudioClient3 *client = nullptr;
    if (!host || FAILED(host->QueryInterface(IID_PPV_ARGS(&client))))
        return host;
    host->Release();
    return reinterpret_cast<IUnknown *>(
        make_forwarder(client, 21, &audio_client_query,
                       {{3, reinterpret_cast<void *>(&initialize)},
                        {7, reinterpret_cast<void *>(&format_supported)},
                        {8, reinterpret_cast<void *>(&mix_format)},
                        {16, reinterpret_cast<void *>(&client_properties)}}));
}
// "\\?\SWD#MMDEVAPI#{endpoint id}#{interface class}": the endpoint, or the default output
IMMDevice *endpoint(IMMDeviceEnumerator *devices, const wchar_t *path) {
    IMMDevice *device = nullptr;
    const std::wstring_view text = path ? path : L"";
    constexpr std::wstring_view marker = L"MMDEVAPI#";
    if (const auto at = text.find(marker); at != text.npos) {
        const auto start = at + marker.size(), end = text.find(L'#', start);
        const std::wstring id(text.substr(start, end - start));
        if (SUCCEEDED(devices->GetDevice(id.c_str(), &device)))
            return device;
    }
    devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    return device;
}
HRESULT WINAPI activate_audio_interface(const wchar_t *path, const GUID &iid, PROPVARIANT *,
                                        IActivateAudioInterfaceCompletionHandler *handler,
                                        IActivateAudioInterfaceAsyncOperation **operation) {
    if (!handler || !operation)
        return E_INVALIDARG;
    handler->AddRef();
    // the title may read the result from the operation it got back here
    auto *const shared = new Activation(E_PENDING, nullptr);
    shared->AddRef();
    std::thread([path = std::wstring(path ? path : L""), iid, handler, shared] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator *devices = nullptr;
        IUnknown *audio = nullptr;
        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&devices));
        if (SUCCEEDED(result)) {
            auto *const device = endpoint(devices, path.c_str());
            result = device ? device->Activate(iid, CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void **>(&audio))
                            : E_NOTFOUND;
            if (device)
                device->Release();
            devices->Release();
        }
        wchar_t name[40];
        StringFromGUID2(iid, name, 40);
        std::fprintf(stderr, "[Runtime] audio interface %ls for %ls: 0x%08lx\n", name, path.c_str(),
                     static_cast<unsigned long>(result));
        if (SUCCEEDED(result) && (iid == __uuidof(IAudioClient) || iid == __uuidof(IAudioClient2) ||
                                  iid == __uuidof(IAudioClient3)))
            audio = audio_client(audio);
        shared->finish(result, audio);
        handler->ActivateCompleted(shared);
        shared->Release();
        handler->Release();
        CoUninitialize();
    }).detach();
    *operation = shared;
    return S_OK;
}
DWORD_PTR WINAPI set_wasapi_thread_affinity_mask(DWORD_PTR mask) { return mask; }
HRESULT WINAPI enable_spatial_audio() { return S_OK; }
void WINAPI nothing() {}

HRESULT WINAPI succeeded() { return S_OK; }
HRESULT WINAPI not_implemented() { return E_NOTIMPL; }

HRESULT WINAPI configure_counters(UINT, UINT, UINT, UINT) { return S_OK; }
UINT64 WINAPI zero() { return 0; }
void WINAPI report_counter(const wchar_t *, float) {}

// module+offset; there are no symbols
using SymbolCallback = BOOL (*)(void *, ULONG_PTR, HRESULT, const wchar_t *, ULONG);
HRESULT WINAPI get_symbol_from_address(int, DWORD count, ULONG_PTR *addresses,
                                       SymbolCallback callback, void *context) {
    if (!addresses || !callback)
        return E_INVALIDARG;
    for (DWORD i = 0; i < count; ++i) {
        const auto location = detail::guest_location(reinterpret_cast<void *>(addresses[i]));
        const auto plus = location.find('+');
        const std::wstring name(location.begin(),
                                location.begin() + (plus == std::string::npos ? 0 : plus));
        const auto offset =
            plus == std::string::npos ? 0 : std::strtoul(location.c_str() + plus + 3, nullptr, 16);
        if (!callback(context, addresses[i], name.empty() ? E_FAIL : S_OK, name.c_str(),
                      static_cast<ULONG>(offset)))
            break;
    }
    return S_OK;
}
BOOL WINAPI query_system_hardware_info() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
} // namespace

void install_system(Registry &registry) {
    registry.forward("mmdevapi.dll", "mmdevapi.dll");
    registry.add("mmdevapi.dll", "ActivateAudioInterfaceAsync", erase(activate_audio_interface));
    registry.add("mmdevapi.dll", "SetWasapiThreadAffinityMask",
                 erase(set_wasapi_thread_affinity_mask));
    registry.add("mmdevapi.dll", "EnableSpatialAudio", erase(enable_spatial_audio));
    for (const auto *name :
         {"DisableBitstreamOut", "RestoreBitstreamOut", "RefreshWasapiDeviceList"})
        registry.add("mmdevapi.dll", name, erase(nothing));

    registry.forward("mfplat.dll", "mfplat.dll");
    registry.add("mfplat.dll", "MFResetDXGIDeviceManagerX", erase(succeeded));
    for (const auto *name : {"MFCreateDxvaSampleRendererX", "MFCreateDXGISurfaceBufferX",
                             "MFCreateNV12ToRGB32ConverterX", "MFCreateRGB32ToNV12ConverterX"})
        registry.add("mfplat.dll", name, erase(not_implemented));

    for (const auto *name : {"ConfigurePMCs", "ConfigureNBPMCs", "ConfigureL2IPMCs"})
        registry.add("pixevt.dll", name, erase(configure_counters));
    for (const auto *name : {"PIXBeginCapture", "PIXEndCapture"})
        registry.add("pixevt.dll", name, erase(succeeded));
    for (const auto *name :
         {"PIXEventsReplaceBlock", "PIXGetCaptureState", "GetCaptureControlState",
          "RegisterForCaptureControl", "UnregisterForCaptureControl", "SetCaptureCallgraphState",
          "SetCaptureFunctionDetailsState", "SetCaptureFunctionSummaryState",
          "SetCaptureInstructionTraceState"})
        registry.add("pixevt.dll", name, erase(zero));
    registry.add("pixevt.dll", "PIXReportCounter", erase(report_counter));

    registry.forward("toolhelpx.dll", "kernel32.dll");
    registry.add("toolhelpx.dll", "GetSymbolFromAddress", erase(get_symbol_from_address));
    registry.add("toolhelpx.dll", "GetSourceLineFromAddress", erase(not_implemented));
    // a title's crash handler writes a dump of itself: none is written, and what it crashed
    // on is in the log
    registry.add(
        "toolhelpx.dll", "MiniDumpWriteDump",
        erase(+[](HANDLE, DWORD, HANDLE, int, void *, void *, void *) -> BOOL { return FALSE; }));
    registry.add("toolhelpx.dll", "QuerySystemHardwareInfo", erase(query_system_hardware_info));
}
} // namespace xv::runtime::durango
