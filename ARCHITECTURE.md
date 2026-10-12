# Architecture

Quick tour of the code. Everything lives under `src/`.

| Directory | Library | What it does |
|---|---|---|
| `formats/pe` | `xs_pe` | PE32+ parser. Takes bytes, returns a JSON report. Never loads anything. |
| `formats/gdk` | `xs_game_config` | Reads `MicrosoftGame.config` |
| `formats/xvd` | `xs_xvd` | Reads the header of an XVD/XVC package |
| `compatibility` | `xs_compat` | Classifies imported DLLs and API names, guesses the build target |
| `guest` | `xs_guest` | Maps an image in memory: sections, relocations, import slots |
| `runtime` | `xs_runtime` | Import resolver and the code that actually runs a guest (Windows) |
| `platform` | `xs_host` | OS wrappers: memory, files, threads, TLS, native modules |
| `graphics` | `xs_graphics`, `xs_vulkan` | Clear pass description and the offscreen Vulkan test backend |
| `frontend` | `xs_frontend` | GLFW + ImGui main window, native file dialog |
| `app` | `xscarlet` | CLI and launch logic |

## Parsing

The PE parser works on a byte span and checks every range before reading it.
RVAs are only resolved to bytes that exist in the file, so a table that points
into a zero-filled tail or into alignment padding is an error. There are limits
on the number of records and string sizes to keep reports bounded.

Parsing and running are separate on purpose. The inspection commands can be
pointed at any file without risk of executing it.

## Loading

`guest::MappedImage` reserves `SizeOfImage` bytes, copies headers and sections
to their RVAs, applies DIR64 relocations and fills the import address table by
calling a resolver for each `(module, symbol)` pair.

By default the image is rebased to wherever the host put it, which is what
running it needs. You can also ask for a fixed guest base (`map --base`), in
which case the result is only good for looking at.

Pages are read-only or read-write according to the section flags. Code
sections become executable only when the runtime asks for it, and a section
that wants to be writable and executable at the same time is rejected.

## Resolving imports

Every import goes through one place (`runtime/`). The order is:

1. `Registry`: our own implementations, keyed by DLL name and symbol. This is
   where `d3d12_xs.dll` and the other console libraries will live.
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
