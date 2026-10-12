# XScarlet

[![build](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml/badge.svg)](https://github.com/sammwyy/xscarlet/actions/workflows/build.yml)

Experimental Xbox Series X|S compatibility layer for Windows, written in C++20.

Series consoles are x86-64 machines running a trimmed-down Windows, so XScarlet
does not emulate a CPU. The idea is closer to a compatibility layer than to a classic emulator:
load the game's executable, run its code natively and provide our own versions
of the console libraries it expects (`d3d12_xs.dll` and friends).

This is a hobby project and it is at a very early stage.

## Status

**It does not run any game yet.** Don't expect it to for a good while.

What exists today:

- PE32+ parser and inspector (imports, delay imports, exports, relocations, TLS, exception tables)
- Detection of the build target from the import table (Series, Xbox One, PC)
- Image loader: sections, relocations, import binding
- Windows runtime that can load and run small test programs through our own
  import resolver, with a stub that tells you which import is missing
- Read-only XVD/XVC package header identification
- A tiny offscreen Vulkan backend used for testing

What is missing is pretty much everything that makes a game boot: the D3D12.X
implementation, the Gaming Runtime, audio, input, storage and package support.
See the [roadmap](ROADMAP.md).

## Building

You need CMake 3.25+, a C++20 compiler and Python 3. Dependencies are fetched
by CMake on the first configure.

### Windows (Visual Studio 2022)

```bat
git clone https://github.com/sammwyy/xscarlet
cd xscarlet
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux

Linux can build the tools and run the tests, but it cannot execute anything.

```sh
sudo apt install build-essential cmake ninja-build python3 xorg-dev libgl-dev libdbus-1-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Extra CMake options:

| Option | What it does |
|---|---|
| `-DXS_VULKAN=ON` | Builds the offscreen Vulkan test (needs the Vulkan SDK) |
| `-DXS_GRAPHICS_TEST=ON` | Desktop window test on X11 (needs `xdotool`, run under `xvfb-run`) |
| `-DXS_SANITIZE=ON` | ASan + UBSan on GCC/Clang |
| `-DBUILD_TESTING=OFF` | Skips the tests |

## Usage

```
xscarlet                           open the window; load a title with File > Open File
xscarlet <file>                    open the window and try to launch a title
xscarlet inspect <file>            dump PE headers and directories as JSON
xscarlet assess <file>             classify imports and guess the build target
xscarlet map <file> [--base HEX]   load the image in memory and print the layout
xscarlet bind <file>               show which imports we can resolve
xscarlet inspect-package <file>    print XVD/XVC header fields
xscarlet inspect-config <file>     read a MicrosoftGame.config
xscarlet exec <file>               run an executable through the runtime (Windows)
```

The window has a File menu with a native open dialog (Ctrl+O), and you can
also drop an executable on it. For now it tells you why the title could not
start, which is always the case. On Linux the dialog comes from
xdg-desktop-portal. `xscarlet --version` prints the version and commit that
the window shows.

`exec` is a developer command. It runs the file's code inside the xscarlet
process with no sandbox at all, so only use it with things you built yourself.
Add `--allow-missing` to start even if some imports are not implemented; the
process exits naming the import as soon as one of them gets called.

## How it works

1. The executable is parsed and its import table is checked. Series builds
   link against `d3d12_xs.dll`, Xbox One builds against `d3d12_x.dll`.
2. The image is mapped, relocated and its imports are bound through a single
   resolver. Each import goes to our own implementation if there is one, then
   to the real Windows function if it is part of the API set games are allowed
   to use, and otherwise to a stub.
3. The entry point runs on its own thread.

More details in [docs/architecture.md](docs/architecture.md). Research notes
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

XScarlet is not affiliated with or endorsed by Microsoft. Xbox and Xbox Series
X|S are trademarks of Microsoft Corporation. The project is written from
public documentation and public source code.

## License

[MIT](LICENSE). Third party bits are listed in [THIRD-PARTY.md](THIRD-PARTY.md).
