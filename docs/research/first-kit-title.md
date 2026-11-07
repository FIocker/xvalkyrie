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
loads textures, starts audio, reads the gamepad and draws: after its opening
video it shows its loading screen as it should.

The opening video is black, and a button skips it. The title uploads three
planes per frame with `UpdateSubresource`, and they hold 0x10 for luma and
0x80 for chroma: no frame was ever decoded. What is known:

- The decoder is VP6 (its jobs are named `vp6_Deco...` and `vp6_mbRo...`,
  the stream's magic is `VP60`), built into the title, and splits each frame into jobs
  for the engine's job system. A last job, which depends on the others,
  hands the frame to the player and gives the frame's context back.
- The decode thread waits for a free frame context (`XS_THREAD_DUMP` shows
  it), so that last job of the first frames never ran. The job system's
  workers sit idle meanwhile.
- A job carries a processor mask, and 0x3f, the six cores a title has, is
  treated apart from any other. The video's jobs take their mask from the
  player's settings. Whether the workers here match those masks is the next
  thing to look at.
- With breakpoints on the decoder (`XS_BREAK`), in a minute the decode thread
  took a frame context 25 times and queued the jobs of each frame, and the
  last job of a frame ran 8 times. The first frame is a key frame and the
  rest depend on the row jobs of the frame before them, through job handles
  kept in the previous context. The frames whose last job never ran are the
  ones to follow: which dependency of theirs stays open.
- Followed further with `XS_BREAK_LIMIT`: the decoder queues frames and the
  last job runs for the first eight, in order. Then it stops because the
  eight decoded frames are never taken: the player's queue of ready frames
  is full.
- The player presents a frame when its number is below the one its clock
  gives (`image+0xd830fb`), and that test never passes. The clock
  (`image+0xd83720`, through the timer object's source) is the position of
  the video's audio voice: it asks the voice how much it has played. The
  voice plays XMA, which the console decodes in its audio processor, and
  nothing decodes it here, so the position stays at zero and the video
  waits on its first frame for ever.
- So the fix is the audio processor's XMA contexts: consume the input the
  title gives and hand back output (silence would do for the picture) at
  the rate of the stream. The title keeps a pending copy of each context
  and applies the fields itself when the update command completes
  (`image+0x119e240`), so the hardware context in the array the runtime
  allocated is where to read them from, not the command's data.
- A first attempt at running the XMA contexts read the fields from the
  update command's data, which is empty in this kit, and changed nothing.
- The title pins its workers one per processor, with masks 0x1 to 0x20 and
  0x80 (`XS_THREAD_TRACE=1` shows them). Reporting six or eight processors
  to it instead of the host's sixteen changed nothing.
- It is not the audio processor: its messages are answered. In this kit
  a completed command's id is 12 bytes into the message, right after the
  type, the dropped count and the time, and the title reads it there.

The launcher counted the imports that the console's own `xg_x.dll` serves as
missing, and refused to start the title that `exec` ran. The system modules
a title came with (`EmbeddedXvd\Windows\System32`, next to the package) are
now part of the plan it shows.

Four console methods of the context are called and ignored without knowing
what they are: at 0x850 (0, pointer to constant data), 0x888 (an object, once
per item of a list), and 0xa10 and 0xa18 around the loop that executes
command lists.

`XS_D3D_SEQ=<frame>` prints every forwarded Direct3D call of one frame in
order, with the range of the bytes an `UpdateSubresource` uploads, which is
how the video's planes were found. `XS_ACP_TRACE=1` prints the first audio
processor commands, `XS_THREAD_DUMP=<frame>` the stack of every thread at
that frame, `XS_THREAD_TRACE=1` the affinity masks the title asks for, and
`XS_BREAK=<rva>,<rva>` the registers each time the title reaches an address
of its image.
