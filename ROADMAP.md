# Roadmap

Rough plan, in the order things need to happen. Windows x64 is the only host
that can execute code. Linux only builds the tools for now.

Nothing here counts as "a game boots" until a real Series title shows its own
output.

## 0. Foundation

- [x] C++20 project, CMake, tests, CI on Windows and Linux
- [x] `xscarlet <file>` entry point with a window that explains why launch failed
- [x] Builds and passes the headless tests with MSVC on Windows 10
- [ ] Window test on Windows

## 1. Executable inspection

- [x] PE32+ headers, sections, imports, exports, relocations, TLS, exception tables
- [x] Delay-load imports
- [x] Import classification and target detection using the tables from
  Microsoft's public `xbdepends` tool
- [ ] Compare against a real Series homebrew build (need one, see below)

## 2. Loader and runtime

- [x] Host memory, files, threads and TLS helpers
- [x] Image mapping: sections, DIR64 relocations, import slots, page protection
- [x] Import resolver: own modules, then allowed Windows APIs, then a stub
- [x] Windows runtime: executable mapping, unwind tables, guest thread, exit codes
- [x] Runs our own test programs (with and without the CRT, C++ exceptions, RTTI, threads)
- [ ] Static TLS
- [ ] Delay-load imports and title-local DLLs
- [ ] `LoadLibrary` / `GetProcAddress` going through the resolver
- [ ] Guest command line
- [ ] Linux execution (far future)

## 3. First homebrew

Blocked: there is no Series executable to test with. Building one needs the
GDK with Xbox Extensions, which is only available to registered developers.
Desktop GDK builds and Xbox One builds are not a substitute.

- [ ] Get a minimal homebrew build with known expected output
- [ ] Find out exactly what it imports (`xscarlet bind`)
- [ ] Implement that and run it

## 4. Graphics

- [x] Offscreen Vulkan clear + readback test (host only, not connected to anything)
- [ ] `d3d12_xs.dll` as our own module, starting with what the first title uses
- [ ] Shaders, resources, presentation into the xscarlet window

## 5. Everything else

- [x] XVD/XVC header identification (read-only, layout from third-party research)
- [ ] Check that layout against a real Series package
- [ ] Gaming Runtime, input, audio, storage
- [ ] More homebrew, regression tracking
- [ ] Retail titles are out of scope until packages can be handled legitimately
