#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unknwn.h>
#include <windows.h>

// Console-only functions. Undocumented ones only return.
namespace xs::runtime::durango {
namespace {
HRESULT WINAPI activate_audio_interface(const wchar_t *device, const GUID &iid, void *parameters,
                                        IUnknown *handler, void **operation) {
    using Activate = HRESULT(WINAPI *)(const wchar_t *, const GUID &, void *, IUnknown *, void **);
    static const auto host = reinterpret_cast<Activate>(
        GetProcAddress(LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32),
                       "ActivateAudioInterfaceAsync"));
    const auto result = host ? host(device, iid, parameters, handler, operation) : E_NOTIMPL;
    std::fprintf(stderr, "[Runtime] ActivateAudioInterfaceAsync(%ls) -> 0x%08lx\n",
                 device ? device : L"(null)", static_cast<unsigned long>(result));
    return result;
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
    registry.add("toolhelpx.dll", "QuerySystemHardwareInfo", erase(query_system_hardware_info));
}
} // namespace xs::runtime::durango
