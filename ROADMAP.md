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
- [ ] Static TLS
- [ ] Delay-load imports and title-local DLLs
- [ ] `LoadLibrary` / `GetProcAddress` going through the resolver
- [ ] Guest command line

## Phase 2: Xbox One runtime

Implement the APIs a test program needs and reproduce its behavior
(`runtime/durango`).

- [ ] Get a minimal Xbox One test program with known expected output
- [ ] Find out exactly what it imports (`xscarlet bind`)
- [ ] Implement those imports and run it
- [ ] Compare its output and exit code against the expected ones

There is no test program yet. Building one needs the GDK with Xbox Extensions
or the older XDK, which are only available to registered developers. A desktop
GDK build is not a substitute.

## Phase 3: Graphics

A first D3D12 to Vulkan translation with working rendering
(`graphics`, used by `d3d12_x.dll` in `runtime/durango`).

- [x] Offscreen Vulkan clear + readback test (host only, not connected to anything)
- [ ] D3D12 device, queues, command lists and resources on Vulkan
- [ ] Shaders
- [ ] `d3d12_x.dll` as our own module, starting with what the test program uses
- [ ] Presentation into the xscarlet window

## Phase 4: Xbox One compatibility

Filesystem, audio, input and the runtime services.

- [x] XVD/XVC header identification (read-only, layout from third-party research)
- [ ] Filesystem and storage paths
- [ ] Audio
- [ ] Input
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
