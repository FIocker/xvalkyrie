# Roadmap

Xbox One (Durango) comes first, Xbox Series X|S (Scarlett) after it. Both are
x86-64 consoles running a trimmed-down Windows, so most of the work is shared
and the Series runtime is built on top of what the Xbox One one needs.

Windows x64 is the only host that can execute code. Linux only builds the
tools for now. Nothing here counts as "a game boots" until a real title shows
its own output.

## Phase 1: PE loader

Load x64 executables: imports, relocations, memory and entry points. Shared by
both consoles (`formats/pe`, `loader`, `runtime/common`).

- [x] PE32+ headers, sections, imports, delay imports, exports, relocations, TLS, exception tables
- [x] Host memory, files, threads and TLS helpers
- [x] Image mapping: sections, DIR64 relocations, import slots, page protection
- [x] Import resolver: own modules, then allowed Windows APIs, then a stub
- [x] Entry point on a guest thread, unwind tables, exit codes (Windows)
- [x] Runs our own test programs (with and without the CRT, C++ exceptions, RTTI, threads)
- [x] Console target detection from the import table (Xbox One, Series, PC)
- [x] Unpacked title folders: executable taken from `AppxManifest.xml`
- [x] Parses and maps a real ERA title (74k relocations, 502 imports)
- [ ] Static TLS
- [x] Title-local DLLs: mapped like the executable, `DllMain` called before the entry point
- [ ] Delay-load imports
- [ ] `LoadLibrary` / `GetProcAddress` going through the resolver
- [ ] Guest command line

## Phase 2: Xbox One runtime

Implement the APIs a test program needs and reproduce its behavior
(`runtime/durango`).

The test program is an unpacked ERA title kept outside the repository, in
`samples/`. What it needs is listed in
[docs/research/era-title.md](docs/research/era-title.md).

- [x] Find out exactly what it imports (`xscarlet bind`)
- [x] `kernelx.dll`: the Win32 subset goes to the host, plus `XMemAllocDefault` / `XMemFreeDefault`
- [x] `combase.dll` and `ws2_32.dll` go to the host, also by ordinal
- [x] `etwplus.dll` as no-ops
- [x] `exec --trace`: first call of every import, last calls and C++ exception type on a fault
- [x] The entry point runs up to the first WinRT class that is missing
- [x] `msvcr110.dll`, `msvcp110.dll`, `vccorlib110.dll` from the host (needs the Visual C++ 2012 redistributable)
- [x] Activation factories: our classes first, a short list of host namespaces, placeholders for the rest
- [x] Placeholders that report the class, interface, method number and caller of every call nobody implements
- [x] Inert console classes for what the title touches before its first frame (transport controls,
  store license, users, network status, downloadable content, party)
- [x] `CoreApplication::Run`, `CoreApplicationView`, `CoreWindow` and `CoreDispatcher` with a host window
- [x] The title's view is created, initialized, loaded, activated and run, up to `D3D11CreateDevice`
- [x] A signed-in user: one `Windows.Xbox.System.User`, the same object everywhere (user
  list, the gamepad's user, the storage space's user), signed in, not a guest, with an id
  and the gamer tag "Player"
- [x] Xbox Live, multiplayer session, presence and chat answer enough for the title to go
  from its main menu to the next screen; nothing goes online
- [x] Session logs: everything xscarlet and the title print also goes to
  `logs/yyyy-mm-dd_hh-mm-ss.log` next to the executable; lines start with `[Launcher]` or
  `[Runtime]`
- [ ] Choose the gamer tag in the options
- [ ] Connected storage that stores: reads say "no such blob", writes succeed and are dropped
- [x] The title draws inside the xscarlet window (16:9 between the menu and the status
  bar); `exec` still gives it a window of its own
- [x] Opening a title does not block the window: it is read on a worker thread, with a
  loading screen until its first frame
- [ ] Closing a running title without closing xscarlet
- [x] DLLs shipped with the title (`xcompress.dll`), loaded from its folder
- [x] Finished async operations and actions (`winrt/async.cpp`)
- [x] `CoreWindow` in the console layout (bounds, dispatcher, activate; the rest unknown)
- [x] Audio engine: `xaudio2_9.dll` from the host, with the console's creation function
- [x] The title's window messages are handled on another thread than the title's: moving
  or resizing the window does not stop it, and a busy title does not freeze the window
- [x] Input (`src/input`): one gamepad for the title, fed by the keyboard or by the host's
  gamepad. Two capture methods, chosen in Options > Input together with the key bindings:
  from the window's events, or read directly from the system (XInput and the keyboard
  state, Windows only). Saved in the per-user `settings.json`
- [x] `Windows.Xbox.Input.Gamepad`: the list of gamepads and the raw reading
- [ ] The controller interface of the gamepad (user pairing, id), vibration
- [ ] More than one gamepad, binding the host gamepad's buttons, mouse
- [ ] Window events (keys, size, visibility): registered, never raised

## Phase 3: Graphics

Two backends behind the console Direct3D modules, chosen with `--backend`:

- `d3d11`: the host's own Direct3D 11. Windows only, and the default there. This
  is the one that gets titles on screen first.
- `vulkan`: exists on Windows and Linux, cannot render a title yet. It is the
  backend D3D12 (later Xbox One titles, all Series titles) and Linux need.

ERA titles like the test program use `d3d11_x.dll` (Direct3D 11.X).

- [x] Backend selection (`graphics/backend`)
- [x] Offscreen Vulkan clear + readback test (host only, not connected to anything)
- [x] `D3D11CreateDevice` from `d3d11_x.dll` on the host's Direct3D 11
- [x] Swap chain for the title's `CoreWindow`, created on the host window
- [x] Direct3D 11.X objects in the console layout in front of the host's: device, context
  (vtable and in-object method table), swap chain, resources, views, states, shaders
- [x] `xg_x.dll`: tile modes and texture sizes, with one plain layout
- [x] The test program creates its renderer and its UI renderer (targets, shaders, states,
  buffers, samplers)
- [x] State the title writes into the context object instead of calling a method (input
  layout, shaders, topology), applied at each draw
- [x] The test program presents frames: splash screens and its main menu scene
- [x] `XS_FRAME_DUMP` writes a presented frame to a file; `XS_D3D_DEBUG` prints what the
  host's debug layer says after each call
- [ ] The methods of the context whose place is still a guess (see `d3d11x/layout.cpp`)
- [ ] Check the picture against the console's, frame by frame
- [ ] Vulkan: device, command submission, resources, shaders (DXBC to SPIR-V)
- [ ] `d3d12_x.dll`

## Phase 4: Xbox One compatibility

Filesystem, audio, input and the runtime services.

- [x] XVD/XVC header identification (read-only, layout from third-party research)
- [ ] Filesystem and storage paths; save games on disk
- [ ] Audio: the engine starts on the host's XAudio2; menu music does not stop in game
- [ ] Input: one gamepad from keyboard or XInput works; vibration, more gamepads, mouse
- [ ] Runtime services (Gaming Runtime, users, app lifecycle)
- [ ] More test programs, regression tracking

## After that: Scarlett

Same executable, second runtime (`runtime/scarlett`).

- [ ] Get a minimal Series test program and list its imports
- [ ] `d3d12_xs.dll` on the shared D3D12 to Vulkan translation
- [ ] What the Series Gaming Runtime adds on top of the Xbox One one
- [ ] Check the XVD layout against a real Series package

## Other

- [x] C++20 project, CMake, tests, CI on Windows and Linux
- [x] `xscarlet <file>` entry point with a window that explains why launch failed
- [ ] Window test on Windows
- [ ] Linux execution (far future)
- [ ] Retail titles are out of scope until packages can be handled legitimately
