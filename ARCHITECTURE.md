# Architecture

Quick tour of the code. Everything lives under `src/`, one static library per
directory, each with its own `CMakeLists.txt`. There is a single executable,
`xscarlet`, that runs both Xbox One and Xbox Series X|S titles: it looks at the
import table of the title and picks the console runtime to use.

```
src/
  core/            settings, log, the Target enum (durango, scarlett)
  platform/        OS wrappers
  formats/         pe, gdk (MicrosoftGame.config), xvd
  compatibility/   import classification, console target detection
  loader/          image mapping
  graphics/        host rendering backends
  runtime/
    common/        import registry and resolver, guest execution
    durango/       Xbox One console modules
    scarlett/      Xbox Series X|S console modules
    console.*      picks durango or scarlett for a title
  frontend/        window
  app/             CLI and launch logic
```

### Shared

Used for every title, whatever console it was built for.

| Directory | Library | What it does |
|---|---|---|
| `core` | `xs_core` | Settings, log and `Target` |
| `platform` | `xs_host` | OS wrappers: memory, files, threads, TLS, native modules |
| `formats/pe` | `xs_pe` | PE32+ parser. Takes bytes, returns a JSON report. Never loads anything. |
| `formats/gdk` | `xs_game_config` | Reads `MicrosoftGame.config` |
| `formats/xvd` | `xs_xvd` | Reads the header of an XVD/XVC package |
| `compatibility` | `xs_compat` | Classifies imported DLLs and API names, detects the console target |
| `loader` | `xs_loader` | Maps an image in memory: sections, relocations, import slots |
| `graphics` | `xs_graphics`, `xs_vulkan` | Clear pass description and the offscreen Vulkan test backend |
| `runtime/common` | `xs_runtime_common` | Import registry and resolver, and the code that actually runs a guest (Windows) |

### Per console

| Directory | Library | What it does |
|---|---|---|
| `runtime/durango` | `xs_runtime_durango` | Xbox One modules (`d3d12_x.dll` and friends) |
| `runtime/scarlett` | `xs_runtime_scarlett` | Xbox Series X\|S modules (`d3d12_xs.dll` and friends) |

Both are empty for now. Each one has a single entry point,
`install(Registry &)`, that adds its modules to the import registry.

### Executable

| Directory | Library | What it does |
|---|---|---|
| `runtime` | `xs_runtime` | `console_registry(Target)`: the registry of one console |
| `frontend` | `xs_frontend` | GLFW + ImGui main window, native file dialog |
| `app` | `xscarlet` | CLI and launch logic |

### Where new code goes

- Works the same on both consoles (the D3D12 to Vulkan translation, audio
  output, input, file access): a shared library, next to `graphics`.
- A console DLL (`d3d12_x.dll`, `d3d12_xs.dll`): its exports go in that
  console's runtime and call into the shared libraries.
- A module both consoles have with the same interface: register it from
  `console_registry` in `runtime/console.cpp` so that both get it.
- Console runtimes do not include each other.

## Target detection

`compat::import_profile` classifies every imported DLL with the tables of
Microsoft's `xbdepends` tool. `d3d12_x.dll`, `dxcompiler_x.dll`, `xg_x.dll`
and the ERA module `kernelx.dll` mean Xbox One (`Target::durango`).
`d3d12_xs.dll`, `dxcompiler_xs.dll` and `xg_xs.dll` mean Series
(`Target::scarlett`). Stock Direct3D DLLs mean a PC build and are refused, as
are images that mix targets. An image with none of them has an unknown target
and gets no console modules.

## Parsing

The PE parser works on a byte span and checks every range before reading it.
RVAs are only resolved to bytes that exist in the file, so a table that points
into a zero-filled tail or into alignment padding is an error. There are limits
on the number of records and string sizes to keep reports bounded.

Parsing and running are separate on purpose. The inspection commands can be
pointed at any file without risk of executing it.

## Loading

`loader::MappedImage` reserves `SizeOfImage` bytes, copies headers and sections
to their RVAs, applies DIR64 relocations and fills the import address table by
calling a resolver for each `(module, symbol)` pair.

By default the image is rebased to wherever the host put it, which is what
running it needs. You can also ask for a fixed guest base (`map --base`), in
which case the result is only good for looking at.

Pages are read-only or read-write according to the section flags. Code
sections become executable only when the runtime asks for it, and a section
that wants to be writable and executable at the same time is rejected.

## Resolving imports

Every import goes through one place (`runtime/common`). The order is:

1. `Registry`: our own implementations, keyed by DLL name and symbol, filled
   by the runtime of the title's console.
2. Runtime overrides, see below.
3. The real Windows export with the same name. This is only allowed for CRT
   DLLs and for system DLLs when the function name is in the list of APIs that
   console titles can use. Ordinals, console DLLs and unknown DLLs are never
   forwarded. DLLs are loaded by name from System32 only.
4. A small generated stub. If the game calls it, the process prints the name
   of the import and exits with `0xE0585301`.

`xscarlet bind` prints which of these each import would get.

## Running

The guest runs inside the xscarlet process on its own thread. Since guest and
host use the same calling convention on Windows, a bound import is just a
function pointer.

Running in-process means a few things have to be faked:

- `GetModuleHandle(NULL)` and `GetModuleFileName` return the guest image
  instead of `xscarlet.exe`.
- `_CxxThrowException` is reimplemented. The CRT looks up the module that owns
  a thrown type in the loader's module list, and a manually mapped image is
  not in it, so `catch` would never match.
- The image's unwind table is registered with `RtlAddFunctionTable`.

Things that do not work yet: static TLS, delay-load imports, DLLs shipped with
the title, and the guest still sees xscarlet's command line. There is no
isolation of any kind.

## Graphics

Nothing from a guest reaches the GPU yet. `xs_vulkan` is a standalone test
that clears an offscreen image and reads it back, to have a known-good
starting point for the real backend. The window (`xs_frontend`) uses OpenGL
through GLFW and only shows launch results for now. It gets the launch logic
from `app` as two callbacks, so it does not link against the runtime.

## Tests

- `tests/*.cpp`: Catch2 unit tests
- `tests/corpus/generate.py`: writes small synthetic PE files used as fixtures
- `tests/guest/`: two tiny Windows programs that the runtime loads and runs
  (Windows only)
