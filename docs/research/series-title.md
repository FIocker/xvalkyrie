# A Series title that starts and draws nothing: where it stands and what to fix next

Notes from one session with a retail Series shooter (unpacked, its executable
is 97 MB, with the console's `System32` in `EmbeddedXvd`). It starts,
makes its device and presents frames at 3840x2160, but every frame is black and a long run
ends with the host removing the device. Nothing here is fixed yet; this is the list to work
from. Addresses are offsets into the image.

## How to run it

```
xvalkyrie exec <title>\Mount --allow-missing --mute
```

What helped, all environment variables:

| Variable | What it gives |
|---|---|
| `XV_SHADER_WHY=1` | Every shader made anew (the shader cache is neither read nor written), and at 256, 512, 1024... shaders a count of what stopped the ones with a stand-in, by stage |
| `XV_D3D_PROBE=<frame>` | The back buffer at that frame as a BMP in the temporary folder, its average colour and how many draws went into it |
| `XV_FRAME_TRACE=1` | The first `ScheduleFrameEventX`, `WaitFrameEventX`, `PresentX` and `GetFrameStatisticsX` calls |
| `XV_D3D_DEBUG=1` | The host's debug layer. The title runs with it, and the device is not removed in 200 s |

A run ends on its own only when it fails: wrap it in `timeout`. Killing the shell that
started it leaves `xvalkyrie.exe` running, and the next build then cannot write the
executable (`LNK1104`): end the process first.

## What was fixed

- **The first crash, at `+0x6863e7`.** After `D3D12XboxCreateDevice` the title asks the
  device for `IDXGIDevice`, takes its adapter, asks the adapter for `IDXGIAdapter2` and then
  that one's parent for `IDXGIFactory2`. Our adapter answered for `IDXGIAdapter` only, and
  the title does not check: it read through a null pointer. The adapter now answers for
  `IDXGIAdapter2` (commit `ff242e0`).
- **Descriptor heap shadows.** The memory below 4 GiB that stands for a heap was never
  given back when the title let go of the heap, and a copied descriptor could be followed to
  a slot that was gone. Both are fixed in the same commit, but this title lets go of no heap
  (the count is in the line the device's failure prints), so it was not what stops it.

## Open: nothing is drawn

The back buffer has 0 draws at frames 200 and 1500, average colour 0. Of the 2048 shaders
the title asks for before that, 46 translate and 2002 get a stand-in that draws nothing.
What stopped them (`XV_SHADER_WHY=1`):

| Reason | Shaders |
|---|---|
| `a resource that is not a descriptor of a table` | 1615 (1066 vertex, 344 pixel, 205 compute) |
| `s_bitset1_b32` | 117 (113 vertex, 4 pixel) |
| `ds_swizzle_b32` | 45 pixel |
| `instruction de54c102` | 36 compute |
| `instruction 04060604` | 10 compute |
| `a buffer whose stride is not known` | 7 |
| `ds_write_b8`, `ds_write_b32` | 4 compute |
| `s_lshl_b64`, `v_illegal` | 2 pixel |
| other single instructions, mostly `0506 06xx` and `0406 06xx`, a few `d409...`, `d413...`, `7da6...` | the rest, 1 to 8 each |

In order of what they would unlock:

1. **Resources that are not descriptors of a table.** Four shaders in five. The translator
   only knows a resource that a shader loads from a descriptor table of the root signature.
   This title has 2 root signatures in all, so its shaders reach their resources another
   way: through a root descriptor, or through a pointer held in a constant buffer or in the
   memory a root parameter points at. To find out: dump a few with `XV_SHADER_CODE=<folder>`,
   disassemble (`llvm-objdump -d --triple=amdgcn --mcpu=gfx1013`, see
   [console-shaders.md](console-shaders.md)) and follow where the base register of the
   failing load comes from, next to the two root signatures the title makes.
2. **`s_bitset1_b32`**: one scalar instruction, 117 shaders.
3. **`ds_swizzle_b32`**: reads another lane of the same quad; in a pixel shader it is how
   derivatives are taken by hand. `ddx`/`ddy` of the value cover the usual patterns.
4. **The `0506 06xx` / `0406 06xx` words.** So many different last bytes with the same
   first three are unlikely to be that many unknown instructions: more likely one
   instruction with an operand in the low byte, or data inside the code range that the
   decoder walks into (constant tables read with `s_getpc_b64`). Check that first; it may be one fix.

Other things seen that will matter once something is drawn:

- `the title writes a 3840x2160 texture's memory, which the host does not give` three times
  at start: the title maps or takes the address of three 4K textures (its back buffers,
  most likely) and writes them from the CPU. What it writes is lost today.
- `a rectangle list is drawn as the triangles of its three corners: half of each rectangle
  is missing`.
- `CreatePlacedResourceX` render targets in pairs at the same virtual address appear
  to share memory on the console. Not checked here.

## Open: the host removes the device

Without the debug layer a run ends with `the host removed the Direct3D 12 device:
0x887a0005`, and the title then faults at `+0x67a8f9` on several threads (it does not
handle the failure; `CreateCommandListX` returns the same code right before).

| Run | Shader cache | Ended |
|---|---|---|
| first, with `--trace` | empty | device removed, time not taken |
| `XV_D3D_PROBE=200` | warm | still running at 300 s, when it was stopped |
| `XV_SHADER_WHY=1` | not used | 203 s, an access violation inside the NVIDIA driver (`nvwgf2umx.dll+0x209bc0`) under `CopyDescriptorsSimple`, called from `resolve` in `decode_written_heaps` from `execute_command_lists` |
| `XV_SHADER_WHY=1`, after the fixes above | not used | 375 s, device removed |
| `XV_D3D_DEBUG=1` | warm | still running at 200 s; an earlier one ran for several minutes |

What is known:

- The reason the host gives is `0x887a0005` itself, which says nothing.
- With the debug layer the device stays. The layer serializes the command queues, and its
  only messages besides clear-value warnings are 13 of this kind: `Simultaneous-access or
  Buffer Resource is still referenced by write|transition_barrier GPU operations in-flight
  on another Command Queue. It is not safe to start write GPU operations now on this Command
  Queue`. So the lead is a race between two of the title's queues on one resource, which the
  console allows or the title orders with something we do not honour.
- `decode_written_heaps` and `upload_placements_d3d12` run inside `ExecuteCommandLists` of
  whichever queue is called, from several threads at once, and write the host's descriptors
  while the title may be writing the shadow. The driver crash came from there. A torn read
  of a descriptor is now turned away, but the heaps are still written from two threads.
- It may also need many shaders made in the same run: the two dated failures were both
  with the cache off. Not established; the warm runs were stopped earlier than 375 s.

What to try, cheapest first:

1. `XV_D3D_DEBUG=3` (the debug layer without serialized queues). If the device goes again,
   the serialization is what hides it, and the layer's messages at that moment say more.
2. `XV_D3D_SYNC=1` (the GPU done with each `ExecuteCommandLists` before the title goes on).
   If the device stays, it is the cross-queue race.
3. A warm-cache run of 10 minutes, to know whether making shaders matters.
4. One lock around `decode_written_heaps` and the uploads.

DRED without the debug layer was tried (breadcrumbs and page faults forced on, no
`EnableDebugLayer`): the title faults 6 s in, calling a null pointer. The breadcrumb
wrapper of command lists does not go with our list proxies. It was taken out again.

## Open: after graphics

- Gaming Runtime calls without an implementation: `XSpeechSynthesizerCreate`
  (XAccessibility slot 9), `XUserGetDefaultAudioEndpointUtf16` (XUserDevice slot 6).
- Audio: 320 XMA2 decoder streams and SHAPE contexts (512, 0, 0, 1024, 512) are connected
  and nothing is decoded. `LoadLibraryEx(MasteringSuite.dll)` fails.
- Network: the title uses `xcurl.dll` and sockets from the start; it retries
  `wss://rta.xboxlive.com/connect` (WinHTTP websocket) and does not get past startup
  without an answer.
- The executable has a `.msvcjmc` section and ships `clang_rt.asan_dynamic-x86_64.dll`, and
  prints `devmode : 1`: it is not a plain release build.

