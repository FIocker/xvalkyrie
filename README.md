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

The executables are `build\win\Release\xvalkyrie.exe`, the window, and `xvboot.exe`
next to it, which runs a title and has the command line. `build.cmd` builds Release
by default; pass `Debug` for a debug build, `--test` to run CTest and `--vulkan`
to build the Vulkan backend as well. Set `JOBS` to control parallel builds
(default: 4).

The Vulkan backend also needs [Meson](https://mesonbuild.com) (`pip install meson`)
and `glslang` (it comes with the Vulkan SDK) on the `PATH`. CMake fetches DXVK at a
fixed commit, applies `cmake/dxvk.patch` and puts `xv_d3d11.dll` and
`xv_dxgi.dll` next to the executables.

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
xvboot exec <file> [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]
                                    run a title in a window of its own
xvboot inspect-manifest <file>      read an AppxManifest.xml
xvboot inspect <file>               dump PE headers and directories as JSON
xvboot assess <file>                classify imports and guess the build target
xvboot map <file> [--base HEX]      load the image in memory and print the layout
xvboot bind <file>                  show which imports we can resolve
xvboot inspect-package <file>       print XVD/XVC header fields
xvboot inspect-config <file>        read a MicrosoftGame.config
xvboot properties <file>            all of the above in one report
```

There are two executables. `xvalkyrie` is the window: the game library and the
options. `xvboot` is what runs a title, in a process and a window of its own; the
window starts it for each title, and it is also the command line. The caption of a
title's window reads `<title> | <video driver> | <frames per second> | XValkyrie
<version> <commit>`. F11 in that window switches between a window and the whole
screen, and the window has the title's own icon. While a title runs, the
xvalkyrie window shows what its process prints, as text that can be selected and
copied. When the title ends the log stays, with a last line that says how: the
exit code, what it means (an exception's name, the runtime's own codes) and how
long the title ran. File > Close App, or Escape, goes back to the library.

Options > Input sets how the title's gamepad is driven: from the key events of
the title's window ("Window"), or read directly from the system ("Direct"),
which also uses a gamepad plugged into the host. The key for each control can
be changed there; a change counts from the next title started. Defaults: left stick WASD, right stick arrows, A Space,
B Backspace, X Q, Y E, Menu Enter, View Tab.

Each session of the window, and each `exec`, writes everything it prints to
`logs/yyyy-mm-dd_hh-mm-ss.log` under the per-user `xvalkyrie` configuration directory
(`%APPDATA%/xvalkyrie`). Controls, the graphics backend, game library folders and
grid/list preference live in `config.ini` there. Lines from the window
start with `[Launcher]`, lines from `xvboot` with `[Boot]` and lines from running
a title with `[Runtime]`. The window and each title it starts write a log each;
the title's is what to attach when reporting a problem.

Every command that takes an executable also takes the folder of an unpacked
Xbox One title or its `AppxManifest.xml`, and uses the executable the manifest
names. Put your own titles in `samples/`, which git ignores.

The window has a File menu with native dialogs for files (Ctrl+O) and folders,
and Options > Game Libraries lets you add, change, remove and rescan game roots.
The start screen is the game library over a backdrop with the pixel-art
XValkyrie logo. It shows unpacked titles with their name, logo (if available)
and console; switch between grid and list, or search by name, in the line
above them. A click selects a title, a double click or Enter starts it. A right
click offers Run, Open Directory, Refresh Cache (reads the title and its pictures
again) and Properties: what the title is, where its image is mapped, the modules
it imports and where each import binds. A title goes by the name of its manifest, read from the package's `resources.pri`
when the manifest only refers to it (`ms-resource:AppName`), and shows its Xbox Live
title ID next to its version; both are in the caption of its window too. The cover of a title is the largest
square picture its manifest names and never the store's logo, unless it has no
other.
XVD/XVC packages appear but
cannot be mounted or played yet. You can
also drop an executable or a title folder on it. A title that can start runs
in a window of its own, which File > Close App (Ctrl+W) or closing that window
ends; otherwise the window says why not.
`xvalkyrie --version` prints the version and commit that the window shows.

Options > Graphics picks what the title's Direct3D calls end up on for the next
title started: "Direct3D 11", the host's own driver (the default), or "Vulkan",
Direct3D 11 translated to Vulkan by `xv_d3d11.dll` and `xv_dxgi.dll`. It also
picks what F11 does: "Borderless" (the default) makes the title's window as large
as the display, "Native" gives the display to the title's swap chain. Native is
there for Direct3D 12 titles; the others, and a host that refuses, get borderless.

`exec` is what the window uses to start a title, and a developer command. It
runs the file's code inside the xvboot process with no sandbox at all, so only use it with things you built yourself.
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
