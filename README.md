# XValkyrie

[![build](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml/badge.svg)](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml)

Experimental Xbox One and Xbox Series X|S compatibility layer for Windows, written in C++20.

Both consoles are x86-64 machines running a trimmed-down Windows, so XValkyrie
does not emulate a CPU. The idea is closer to a compatibility layer than to a classic emulator:
load the game's executable, run its code natively and provide our own versions
of the console libraries it expects (`d3d12_x.dll` on Xbox One, `d3d12_xs.dll`
on Series, and friends).

Xbox One (Durango) is the first target and Series (Scarlett) comes after it.
One executable handles both: the loader, the import resolver and the graphics
backend are shared, and each console has its own runtime on top of them.

This is a hobby project and it is at a very early stage.

## Status

**It does not run games.** One Xbox One title used as the test program gets as
far as drawing its splash screens and main menu on Windows, with no input, no
saves and nobody signed in. Don't expect more for a good while.

What exists today:

- PE32+ parser and inspector (imports, delay imports, exports, relocations, TLS, exception tables)
- Detection of the build target from the import table (Series, Xbox One, PC)
- Image loader: sections, relocations, import binding
- Windows runtime that can load and run small test programs through our own
  import resolver, with a stub that tells you which import is missing
- Unpacked Xbox One (ERA) title folders: reads `AppxManifest.xml` and finds the executable
- Read-only XVD/XVC package header identification
- Two graphics backends for Direct3D 11.X titles: the host's Direct3D 11, or
  Direct3D 11 on Vulkan through a patched [DXVK](https://github.com/doitsujin/dxvk)
- A tiny offscreen Vulkan backend used for testing

What is missing is pretty much everything that makes a game boot: both console
runtimes are empty. No D3D12.X implementation, Gaming Runtime, audio, input,
storage or package support.

The plan, see the [roadmap](ROADMAP.md):

1. PE loader: x64 executables, imports, relocations, memory, entry points
2. Xbox One runtime: the APIs a test program needs
3. Graphics: a first D3D12 to Vulkan translation
4. Xbox One compatibility: filesystem, audio, input, runtime services

Series comes after those.

## Building

XValkyrie builds and runs on 64-bit Windows. You need Visual Studio 2022
with the "Desktop development with C++" workload, CMake 3.25+ and Python 3.
Dependencies are fetched by CMake on the first configure.

```bat
build.cmd --test
```

The executable is `build\win\Release\xvalkyrie.exe`. `build.cmd` builds Release
by default; pass `Debug` for a debug build, `--test` to run CTest and `--vulkan`
to build the Vulkan backend as well. Set `JOBS` to control parallel builds
(default: 4).

The Vulkan backend also needs [Meson](https://mesonbuild.com) (`pip install meson`)
and `glslang` (it comes with the Vulkan SDK) on the `PATH`. CMake fetches DXVK at a
fixed commit, applies `cmake/dxvk.patch` and puts `xv_d3d11.dll` and
`xv_dxgi.dll` next to `xvalkyrie.exe`.

Extra CMake options:

| Option | What it does |
|---|---|
| `-DXV_DXVK=ON` | Builds Direct3D 11 on Vulkan for the `vulkan` backend (needs Meson and glslang) |
| `-DXV_VULKAN=ON` | Builds the offscreen Vulkan test (needs the Vulkan SDK) |
| `-DBUILD_TESTING=OFF` | Skips the tests |

## Usage

```
xvalkyrie                           open the window; load a title with File > Open File
xvalkyrie <file>                    open the window and try to launch a title
xvalkyrie <folder>                  same, for an unpacked title (folder with AppxManifest.xml)
xvalkyrie inspect-manifest <file>   read an AppxManifest.xml
xvalkyrie inspect <file>            dump PE headers and directories as JSON
xvalkyrie assess <file>             classify imports and guess the build target
xvalkyrie map <file> [--base HEX]   load the image in memory and print the layout
xvalkyrie bind <file>               show which imports we can resolve
xvalkyrie inspect-package <file>    print XVD/XVC header fields
xvalkyrie inspect-config <file>     read a MicrosoftGame.config
xvalkyrie exec <file> [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]
                                   run it through the runtime (Windows)
```

Options > Input sets how the title's gamepad is driven: from the window's key
events ("Window"), or read directly from the system ("Direct"),
which also uses a gamepad plugged into the host. The key for each control can
be changed there. Defaults: left stick WASD, right stick arrows, A Space,
B Backspace, X Q, Y E, Menu Enter, View Tab.

Each session of the window, and each `exec`, writes everything it prints to
`logs/yyyy-mm-dd_hh-mm-ss.log` under the per-user `xvalkyrie` configuration directory
(`%APPDATA%/xvalkyrie`). Controls, the graphics backend, game library folders and
grid/list preference live in `config.ini` there. Lines from the window
start with `[Launcher]`, lines from running a title with `[Runtime]`. That
file is what to attach when reporting a problem.

Every command that takes an executable also takes the folder of an unpacked
Xbox One title or its `AppxManifest.xml`, and uses the executable the manifest
names. Put your own titles in `samples/`, which git ignores.

The window has a File menu with native dialogs for files (Ctrl+O) and folders,
and Options > Game Libraries lets you add, change, remove and rescan game roots.
The start screen is the game library over a backdrop with the pixel-art
XValkyrie logo. It shows unpacked titles with their name, logo (if available)
and console; switch between grid and list, or search by name, in the line
above them. A click selects a title, a double click or Enter starts it.
XVD/XVC packages appear but
cannot be mounted or played yet. You can
also drop an executable or a title folder on it. A title that can start runs
inside the window, after a loading screen; otherwise the window says why not.
`xvalkyrie --version` prints the version and commit that the window shows.

Options > Graphics picks what the title's Direct3D calls end up on for the next
title started: "Direct3D 11", the host's own driver (the default), or "Vulkan",
Direct3D 11 translated to Vulkan by `xv_d3d11.dll` and `xv_dxgi.dll`.

`exec` is a developer command. It runs the file's code inside the xvalkyrie
process with no sandbox at all, so only use it with things you built yourself.
Add `--allow-missing` to start even if some imports are not implemented; the
process exits naming the import as soon as one of them gets called. `--trace`
prints the first call of every imported function, and the last ones before a
fault. `--mute` silences the title. `--backend` overrides the backend of Options > Graphics:
`d3d11` is the host's Direct3D 11, `vulkan` is Direct3D 11 on Vulkan.

## How it works

1. The executable is parsed and its import table is checked. Xbox One builds
   link against `d3d12_x.dll`, Series builds against `d3d12_xs.dll`. That
   decides which console runtime is used.
2. The image is mapped, relocated and its imports are bound through a single
   resolver. Each import goes to our own implementation if there is one, then
   to the real Windows function if it is part of the API set games are allowed
   to use, and otherwise to a stub.
3. The entry point runs on its own thread.

More details in [ARCHITECTURE.md](ARCHITECTURE.md). Research notes
live in [docs/research](docs/research).

## FAQ

**Can I play games with this?**
No.

**When?**
No idea. This is a spare time project.

**Where do I get games or keys?**
Not here. XValkyrie does not include or link to games, keys, firmware or SDK
files, and it has no way of decrypting retail packages. Please don't ask.

## Contributing

Issues and pull requests are welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

XValkyrie is not affiliated with or endorsed by Microsoft. Xbox, Xbox One and
Xbox Series X|S are trademarks of Microsoft Corporation. The project is written from
public documentation and public source code.

## License

[MIT](LICENSE). Third party bits are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
