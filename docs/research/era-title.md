# What an Xbox One ERA title needs

Notes from looking at one unpacked ERA title (XDK era, built with Visual Studio
2012, C++/CX). Everything here comes from its own import table, its strings
and its `AppxManifest.xml`. `xscarlet assess`, `bind` and `inspect-manifest`
print the same information.

## Layout on disk

```
<title>/Mount/AppxManifest.xml     Executable and EntryPoint of the application
<title>/Mount/<name>.exe           PE32+ x64, console subsystem
<title>/Mount/*.dll                DLLs shipped with the title
```

The manifest has `mx:OSName` = `era`. The executable has relocations, an
exception table, no static TLS and no delay-load imports, so the loader can
already map it.

## Import table

| Module | Imports | What it is |
|---|---|---|
| `kernelx.dll` | 79 | The ERA kernel32. All but two names are ordinary Win32 functions (`CreateFileW`, `VirtualAlloc`, `CreateThread`...). The two others are `XMemAllocDefault` and `XMemFreeDefault`. |
| `MSVCR110.dll`, `MSVCP110.dll` | 177, 132 | Visual C++ 2012 runtime |
| `vccorlib110.dll` | 60 | C++/CX runtime: `Platform::Object`, exceptions, event sources, `GetActivationFactoryByPCWSTR` |
| `combase.dll` | 19 | `HSTRING` functions, `CoCreateInstance`, marshaling helpers |
| `WS2_32.dll` | 22 | Winsock, mostly by ordinal |
| `d3d11_x.dll` | 1 | `D3D11CreateDevice`. Everything else is COM methods on the device and context it returns. |
| `xg_x.dll` | 3 | Texture layout: `XGCreateTexture2DComputer`, `XGComputeOptimalTileMode`, `XGComputeOptimalDepthStencilTileModes` |
| `EtwPlus.dll` | 4 | Event tracing: `EtxRegister`, `EtxEventWrite`, `EtxFillCommonFields_v7`, `EtxUnregister` |
| `xcompress.dll` | 5 | Shipped with the title. LZX compression: `XMemCreateDecompressionContext`, `XMemDecompress`... |

It also loads `xaudio2_9.dll` by name at run time.

## WinRT

The import table is the small part. The entry point named in the manifest is a
C++/CX class, and the title talks to the system through WinRT classes that it
activates by name through `vccorlib110.dll` and `combase.dll`. Class names
found in the executable, by namespace:

| Namespace | Used for |
|---|---|
| `Windows.ApplicationModel.Core`, `Windows.UI.Core` | `CoreApplication::Run`, `CoreWindow`, activation, suspend |
| `Windows.Foundation`, `Windows.Foundation.Collections` | Async operations, `PropertyValue`, `Uri`, collections |
| `Windows.Storage.Streams`, `Windows.Data.Json` | Buffers, readers and writers, JSON |
| `Windows.Xbox.Input` | `Gamepad`, `Controller` |
| `Windows.Xbox.System` | `User`, sign in and out, audio devices |
| `Windows.Xbox.Storage` | `ConnectedStorageSpace` (save games) |
| `Windows.Xbox.UI`, `Windows.Xbox.ApplicationModel` | System UI, help, store |
| `Windows.Xbox.Management.Deployment` | Downloadable content packages |
| `Windows.Xbox.Networking`, `Windows.Xbox.Multiplayer` | Secure device associations, party |
| `Windows.Xbox.Media`, `Windows.Kinect` | Transport controls, Kinect sensor |
| `Windows.Networking.Connectivity`, `Windows.ApplicationModel.Store` | Network status, licensing |
| `Microsoft.Xbox.Services.*`, `Microsoft.Xbox.GameChat` | Xbox Live and chat, implemented by DLLs shipped with the title |

## Consequences

- An ERA title is a WinRT application. Running the entry point of the PE is
  not enough: it calls `CoreApplication::Run` with its own view source and
  expects a `CoreWindow` and a dispatcher back.
- Graphics is Direct3D 11.X, not D3D12.
- Most of `kernelx.dll` can go straight to the host. The WinRT classes and
  Direct3D 11.X are the actual work.
- Xbox Live, multiplayer, Kinect and the store can report "not available" for
  a long time before anything needs them.

## Start-up sequence

What the title does before its first frame, in order, as seen with
`xscarlet exec --allow-missing`:

1. Static initializers: creates a `Windows.Xbox.Media.GameTransportControls`
   and enables its buttons; reads the license from
   `Windows.ApplicationModel.Store.CurrentApp`; reads
   `Windows.Xbox.System.User.Users` and registers for user events; registers
   for network status, store purchases and downloadable content events.
2. `main` calls `CoreApplication::Run` with its view source.
3. The view registers for a console-only `CoreApplication` event during
   `Initialize`, and sets two `Windows.Xbox.Multiplayer.PartyConfig` values
   when it is activated.
4. Inside the view's `Run` it calls `D3D11CreateDevice` from `d3d11_x.dll`.

Interface IDs and method numbers of the console classes involved are in
`src/runtime/durango/winrt/inert.cpp`. They were read from the title's own
call sites: the IID it asks for, the vtable slot it calls, the arguments it
passes.

## Direct3D 11.X

Seen from the title's code after `D3D11CreateDevice`:

- It asks the device for `ID3D11Device1`, the context for `ID3D11DeviceContext1`
  and goes device -> `IDXGIDevice1` -> adapter -> `IDXGIFactory2` ->
  `CreateSwapChainForCoreWindow` (1920x1080, R8G8B8A8, 2 buffers, flip
  sequential). All with the desktop interface IDs. The stub `d3d11_x.dll` in the
  system image exports every `IID_*` as data, which gives the IDs of the
  console-only interfaces too (`ID3D11DeviceX`, `ID3D11DeviceContextX`...).
- Reference counting goes through the vtable, as in ordinary COM.
- The device methods used so far sit in their desktop slots
  (`CreateRenderTargetView` is slot 9).
- Resources do not. The title calls `ID3D11Texture2D::GetDesc` at slot 13, and
  on the desktop it is slot 10: there are three more methods before it, in
  `ID3D11DeviceChild` and/or `ID3D11Resource`. Same interface ID, different
  vtable.
- The context is called at slots above 150, and the desktop
  `ID3D11DeviceContext1` ends at 133, so its layout is not the desktop one
  either.

So the host's objects cannot be handed to the title as they are. What the
title's calls have shown so far:

- Device children have two more methods after `ID3D11DeviceChild`'s four, and
  resources one more after `ID3D11Resource`'s three. `GetDesc` of a texture or
  a buffer is slot 13, `GetResource` of a view is slot 9.
- The device has the desktop slots up to at least 23 (`CreateSamplerState`)
  and one extra method somewhere before `GetImmediateContext`, which is 41.
- `ID3D11PerformanceDeviceX` and `ID3D11PerformanceContextX` are asked for by
  the UI renderer and then used as the device and the context.
- The context is called through function pointers stored in the object. In
  units of 8 bytes: `VSSetConstantBuffers` 131, `PSSetSamplers` 134, `Map`
  138, `Unmap` 139, `PSSetConstantBuffers` 140, `IASetVertexBuffers` 142,
  `IASetIndexBuffer` 143, `VSSetShaderResources` 148, `VSSetSamplers` 149,
  `OMSetRenderTargets` 156, `RSSetViewports` 167, `UpdateSubresource` 171,
  `ClearRenderTargetView` 173, `OMGetRenderTargets` 212. That is desktop
  order + 124 up to 19 and + 123 from 25 on, so one desktop method between
  20 and 24 has no entry.
- Two of those have another signature than on the desktop:
  `XXSetShaderResources(address of the view pointer, slot, packet header)`,
  one view per call, and `IASetIndexBuffer(index size, buffer, offset)`.
- `D3D11_TEXTURE2D_DESC` is 8 bytes longer (two ESRAM fields), has console
  bits in `MiscFlags` and uses quality `0xffffffff` with one sample.
- `xg_x.dll` is only used to get the total size of three render targets
  (`XGCreateTexture2DComputer`, then the third method of the computer, which
  fills a 0xd40-byte layout structure whose first field is that size).
- Some context setters never call anything. The headers inline them: the title
  stores the value in the context object and ORs dirty bits into the dword at
  `+0x168`. Input layout at `+0x178` (bits `0x89`), vertex shader at `+0x180`
  (`0x91`), pixel shader at `+0x188` (`0x121`), topology at `+0x170` (`0x46`)
  as a 64-bit hardware value taken from a table in the title indexed by
  `D3D11_PRIMITIVE_TOPOLOGY` (`0x9ffc0009e00` is a triangle list,
  `0xdffc0009e00` a triangle strip). The layout and shader setters first test
  bit 1 of the byte at `+0x390` and call the method instead if it is set.
- `DrawIndexed` takes `(index count << 32 | first index, base vertex)`.
- `Present` is the desktop `IDXGISwapChain::Present`.

## Other things the title needs before its menu

- `CoreWindow` is not the desktop `ICoreWindow`: `get_Bounds` is the first
  method, `get_Dispatcher` the second, `Activate` the sixth, and it registers
  four events at methods 17, 19, 23 and 25.
- Audio is the Miles Sound System on XAudio2. It loads `xaudio2_9.dll` by name
  and asks it for `CreateXAudio2Object` (engine out, flags, processor, one
  more argument), which the desktop DLL calls `XAudio2Create`.
- `ConnectedStorageSpace` hands out an `IAsyncOperation`; the title sets its
  Completed handler and goes on from there.

## Gamepad

- `Windows.Xbox.Input.Gamepad` statics (`8c4c1bcd-...`): the list of gamepads
  is the first method, then two event registrations (added, removed).
- The title uses each gamepad through interface `9776ff56-...`, whose third
  method fills a 40-byte reading: a 64-bit timestamp, the buttons as 32 bits,
  then six floats (left trigger, right trigger, left stick x and y, right
  stick x and y). Buttons: Menu 0x4, View 0x8, A 0x10, B 0x20, X 0x40, Y 0x80,
  D-pad up/down/left/right 0x100/0x200/0x400/0x800, left/right shoulder
  0x1000/0x2000, left/right stick click 0x4000/0x8000. Triggers count as
  pressed above a threshold, sticks are scaled to 16-bit and get a dead zone
  in the title.
- It also asks the gamepad for a controller interface (`7400624f-...`) and
  reads two properties from the first one; both are answered with nothing.
