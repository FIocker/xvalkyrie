# XScarlet

[![build](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml/badge.svg)](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml)

Experimental Xbox One and Xbox Series X|S compatibility layer for Windows, written in C++20.

Both consoles are x86-64 machines running a trimmed-down Windows, so XScarlet
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

You need CMake 3.25+, a C++20 compiler and Python 3. Dependencies are fetched
by CMake on the first configure. Both scripts build Release by default; pass
`Debug` for a debug build and `--test` to run CTest. Set `JOBS` to control
parallel builds (default: 4).

### Windows (Visual Studio 2022)

Install the "Desktop development with C++" workload, then from the repository:

```bat
build.cmd --test
```

The executable is `build\win\Release\xscarlet.exe`. Use `build.cmd Debug --test`
for a debug build.

### Linux

Linux can build the tools and run the tests, but it cannot execute anything.

```sh
sudo apt install build-essential cmake ninja-build python3 xorg-dev libgl-dev libdbus-1-dev
./build.sh --test
```

The executable is `build/Release/xscarlet`. Use `./build.sh Debug --test` for a
debug build. The scripts can also be run from outside the repository directory.

Extra CMake options:

| Option | What it does |
|---|---|
| `-DXS_VULKAN=ON` | Builds the offscreen Vulkan test (needs the Vulkan SDK) |
| `-DXS_SANITIZE=ON` | ASan + UBSan on GCC/Clang |
| `-DBUILD_TESTING=OFF` | Skips the tests |

## Usage

```
xscarlet                           open the window; load a title with File > Open File
xscarlet <file>                    open the window and try to launch a title
xscarlet <folder>                  same, for an unpacked title (folder with AppxManifest.xml)
xscarlet inspect-manifest <file>   read an AppxManifest.xml
xscarlet inspect <file>            dump PE headers and directories as JSON
xscarlet assess <file>             classify imports and guess the build target
xscarlet map <file> [--base HEX]   load the image in memory and print the layout
xscarlet bind <file>               show which imports we can resolve
xscarlet inspect-package <file>    print XVD/XVC header fields
xscarlet inspect-config <file>     read a MicrosoftGame.config
xscarlet exec <file> [--allow-missing] [--trace] [--mute] [--backend d3d11|vulkan]
                                   run it through the runtime (Windows)
```

Options > Input sets how the title's gamepad is driven: from the window's key
events ("Window"), or read directly from the system ("Direct", Windows only),
which also uses a gamepad plugged into the host. The key for each control can
be changed there. Defaults: left stick WASD, right stick arrows, A Space,
B Backspace, X Q, Y E, Menu Enter, View Tab.

Each session of the window, and each `exec`, writes everything it prints to
`logs/yyyy-mm-dd_hh-mm-ss.log` under the per-user `xscarlet` configuration directory
(`%APPDATA%/xscarlet` on Windows, `$XDG_CONFIG_HOME/xscarlet` or
`~/.config/xscarlet` on Linux). Controls, game library folders and grid/list
preference live in `config.ini` there. Lines from the window
start with `[Launcher]`, lines from running a title with `[Runtime]`. That
file is what to attach when reporting a problem.

Every command that takes an executable also takes the folder of an unpacked
Xbox One title or its `AppxManifest.xml`, and uses the executable the manifest
names. Put your own titles in `samples/`, which git ignores.

The window has a File menu with native dialogs for files (Ctrl+O) and folders,
and Options > Game Libraries lets you add, change, remove and rescan game roots.
The start screen is the game library over a backdrop with the pixel-art
XScarlet logo. It shows unpacked titles with their name, logo (if available)
and console; switch between grid and list, or search by name, in the line
above them. A click selects a title, a double click or Enter starts it.
XVD/XVC packages appear but
cannot be mounted or played yet. You can
also drop an executable or a title folder on it. A title that can start runs
inside the window, after a loading screen; otherwise the window says why not. On Linux the dialog comes from
xdg-desktop-portal. `xscarlet --version` prints the version and commit that
the window shows.

`exec` is a developer command. It runs the file's code inside the xscarlet
process with no sandbox at all, so only use it with things you built yourself.
Add `--allow-missing` to start even if some imports are not implemented; the
process exits naming the import as soon as one of them gets called. `--trace`
prints the first call of every imported function, and the last ones before a
fault. `--mute` silences the title. `--backend` picks what Direct3D calls end up on: `d3d11` is the host's
Direct3D 11 (Windows only, the default there), `vulkan` exists on every host
but cannot render a title yet.

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
Not here. XScarlet does not include or link to games, keys, firmware or SDK
files, and it has no way of decrypting retail packages. Please don't ask.

**Linux support?**
Maybe some day. Windows comes first because most of the system API is already
there.

## Contributing

Issues and pull requests are welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

XScarlet is not affiliated with or endorsed by Microsoft. Xbox, Xbox One and
Xbox Series X|S are trademarks of Microsoft Corporation. The project is written from
public documentation and public source code.

## License

[MIT](LICENSE). Third party bits are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
