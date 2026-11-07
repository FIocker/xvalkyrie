# A title built with the first development kits

Notes from getting one Frostbite title built with an early kit to run.
Everything here comes from its call sites and from what it does when a call
goes wrong. It uses Direct3D 11.X, and its interfaces are not laid out like
those of the titles looked at before, which were built with later kits.

## Telling the two apart

The title creates its device with `D3D11XCreateDeviceX`, whose parameters
start with the SDK version of the kit: 0x1000b here. The later titles pass
0x20011 or 0x20012, to that function or to `D3D11CreateDevice`.
`create_device` treats anything below 0x20000 as a first-kit title and
switches the layouts below.

## What differs

| Interface | Later kits | First kits |
|---|---|---|
| `IDXGIObject` and everything derived from it | The desktop's slots | One more method at slot 5: `GetParent` is 7, `IDXGIDevice::GetAdapter` 8, `IDXGISwapChain::GetBuffer` 10 |
| Device children | Two console methods after `ID3D11DeviceChild`'s (slots 7 and 8), own methods from 9 | One console method (slot 7), own methods from 8: `ID3D11Resource::GetType` is 8 |
| Device | Desktop slots, one console method at 37, console methods after | The same, as far as the title uses it (`CreatePlacementBuffer` at 62) |
| Context, table inside the object | Desktop method `n` at pointer `n + 124` (`n + 123` from 25 on, there is no `IASetPrimitiveTopology`) | One pointer earlier: `n + 123` and `n + 122` |
| Context, console methods in that table | From pointer 265 | Three pointers earlier: two of the later methods do not exist yet |
| `XXSetShaderResources` in that table | One view per call: (address of the view, slot, packet header) | The desktop's arguments: (start, count, views) |

What is the same: the state the title writes into the context instead of
calling (dirty bits at 0x168, topology at 0x170, input layout, vertex and
pixel shader after it), the command buffer pointers at 0x50 and 0x58, the
shader setters that take the shader alone, `IASetIndexBuffer` with the
hardware index size first, and `DrawIndexed` with count and start in one
argument.

The evidence for each row:

- **DXGI.** The title queried the device for `IDXGIDevice1` and called slot
  8 with one out pointer, then called slot 7 of the adapter with an IID and
  an out pointer. On the swap chain it called slot 10 with (0, IID, out).
- **Device children.** One call site calls slot 8 of every resource with an
  out pointer and switches on the result. With slot 8 ignored the title
  read an uninitialized type and asked a buffer for `ID3D11Texture2D`.
- **Context table.** `OMSetRenderTargets(1, &view, depth)` came in at
  pointer 155 and `RSSetViewports(1, &viewport)` at 166, where later titles
  have them at 156 and 167. `IASetVertexBuffers` and `Draw`, below 24,
  were one pointer early too.
- **Console methods.** Three call sites fix the offset: the index buffer
  setter that takes (index size, buffer, address) at 0x9d8, and the pair
  around inline packets at 0xa50 (called when the write pointer has reached
  the end) and 0xa58 (called with a packet and its end). Later titles have
  the three at 0x9f0, 0xa68 and 0xa70.
- **Shader resources.** The call sites pass 0, a count and an array of up to
  128 views.

`ID3DUserDefinedAnnotation` is asked of the context under the console's IID
(`b2daad8b-03d4-4dbf-95eb-32ab4b63d0ab`). On the console that interface is the
context itself, with the PIX methods of its table.

## Paths

The title opens files from the root of the current drive (`\Data\layout.toc`),
which on the console is the package. Such paths are mapped to the package
folder, also in the ANSI functions (`CreateFileA`, `FindFirstFileA`,
`FindFirstFileExA`, `GetFileAttributesA`, `GetFileAttributesExA`), which it
uses for most of its file access.

## WinRT it needs before the first frame

| Class | What it asks | Answer |
|---|---|---|
| `PackageTransferManager` | `Current`, an event | The one manager, never raised |
| `SecureDeviceAssociationTemplate` | `Templates` | Empty |
| `Windows.Kinect.KinectSensor` | `GetDefault`, `Open`, `IsAvailable`, `BodyFrameSource` | A sensor that is never available. It must not be null: the title calls `Open` on it unchecked |
| `BodyFrameSource`, `BodyFrameReader` | `OpenReader`, frame event, latest frame | A reader that never has a frame |
| `BodyController` | The list, two events | Empty |
| `SpeechRecognizerFactory`, `SpeechRecognizer` | `Create`, a flag, seven events | A recognizer that hears nothing |
| `GestureRecognizer` | Settings, three events | Accepted |
| `SmartGlassDevice`, `SmartGlassDeviceWatcher` | `FindAllAsync`, two events | No devices |
| `NetworkInformation` | `GetInternetConnectionProfile` | None |

The audio processor's fifth method is `GetNumMessages`, which the title polls
every frame.

## State

The title starts, creates its device and swap chain, compiles its shaders,
loads textures, starts audio, reads the gamepad and presents frames without
a fault. The frames are black. The host's debug layer says draws run with no
render target bound, which is where to look next: the title records on
deferred contexts and plays the lists back on the immediate one.

Four console methods of the context are called and ignored without knowing
what they are: at 0x850 (0, pointer to constant data), 0x888 (an object, once
per item of a list), and 0xa10 and 0xa18 around the loop that executes
command lists.
