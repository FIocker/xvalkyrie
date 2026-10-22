# Architecture

Quick tour of the code. Everything lives under `src/`, one static library per
directory, each with its own `CMakeLists.txt`. There is a single executable,
`xscarlet`, that runs both Xbox One and Xbox Series X|S titles: it looks at the
import table of the title and picks the console runtime to use.

Two words are used with one meaning each. The **host** is the machine xscarlet
runs on (Windows, Linux). A **console** is what the title was built for:
Xbox One (`durango`) or Xbox Series X|S (`scarlett`).

```
src/
  core/            the Target enum (durango, scarlett), log, settings
  host/            OS wrappers: memory, files, threads, native modules, session log
    windows/  linux/
  formats/         pe, gdk (MicrosoftGame.config), appx (AppxManifest.xml), xvd
  analysis/        import classification, console target detection
  loader/          image mapping
  backends/        what a title's output and input end up on, per host technology
    graphics/      backend selection; d3d11/ (Windows), vulkan/ (every host)
    audio/         wasapi/ (Windows), null/
    input/         the emulated gamepad; xinput/ (Windows), null/
    surface/       the window a title presents into; win32/, null/
  runtime/         the console side: what a title's imports are bound to
    common/        import registry and resolver, guest execution
    modules/       console modules both consoles have
    durango/       Xbox One only
    scarlett/      Xbox Series X|S only
    console.*      picks durango or scarlett for a title
  launcher/        whether a title can start, game library, saved configuration
  app/             main.cpp: the CLI
    ui/            the window
  tools/           developer executables
```

### Layers

A directory only includes from the ones above it in that list, with these
rules on top:

- `backends/` knows nothing about consoles. No `Target`, no console DLL names,
  no console object layouts.
- `runtime/` is the only place that knows what a console DLL looks like. It
  reaches the host through `host/` and `backends/`, never the other way round.
  The exceptions are the files that are the host side of running a guest and
  only build there: `runtime/common/windows/` and the Windows-only parts of
  `runtime/durango` (see "Windows-only code" below).
- `runtime/durango` and `runtime/scarlett` do not include each other. What
  both need goes to `runtime/modules`.
- `launcher/` has no window and prints nothing. `app/ui` uses `launcher`;
  `launcher` never includes `app`.
- A file that only builds on one host sits in a subdirectory named after the
  technology it uses (`win32/`, `wasapi/`, `xinput/`, `d3d11/`, `vulkan/`),
  or `windows/` / `linux/` when it is the OS as a whole. `null/` is the
  implementation for hosts that have none. New code picks the one to build
  in the `CMakeLists.txt` of the directory rather than with `#ifdef`.

### Shared

Used for every title, whatever console it was built for.

| Directory | Library | What it does |
|---|---|---|
| `core` | `xs_core` | `Target`, log and settings |
| `host` | `xs_host` | OS wrappers: memory, files, threads, TLS, native modules, session log |
| `formats/pe` | `xs_pe` | PE32+ parser. Takes bytes, returns a JSON report. Never loads anything. |
| `formats/gdk` | `xs_game_config` | Reads `MicrosoftGame.config` |
| `formats/appx` | `xs_appx` | Reads `AppxManifest.xml`, which names the executable of an unpacked ERA title |
| `formats/xvd` | `xs_xvd` | Reads the header of an XVD/XVC package |
| `analysis` | `xs_analysis` | Classifies imported DLLs and API names, detects the console target |
| `loader` | `xs_loader` | Maps an image in memory: sections, relocations, import slots |
| `backends/graphics` | `xs_graphics` | Which backend is selected, and the clear pass description |
| `backends/graphics/d3d11` | `xs_graphics_d3d11` | The host's Direct3D 11: device creation, frame dump (Windows) |
| `backends/graphics/vulkan` | `xs_vulkan` | The offscreen Vulkan test backend |
| `backends/audio` | `xs_audio` | Sound output; so far only muting the process |
| `backends/input` | `xs_input` | The emulated gamepad: capture methods, key bindings |
| `backends/surface` | `xs_surface` | `TitleWindow`, the window a title presents into |
| `runtime/common` | `xs_runtime_common` | Import registry and resolver, and the code that actually runs a guest (Windows) |
| `runtime/modules` | `xs_runtime_modules` | Console modules with the same interface on both consoles |

### Per console

| Directory | Library | What it does |
|---|---|---|
| `runtime/durango` | `xs_runtime_durango` | Xbox One modules (`d3d12_x.dll` and friends) |
| `runtime/scarlett` | `xs_runtime_scarlett` | Xbox Series X\|S modules (`d3d12_xs.dll` and friends) |

Each one has a single entry point, `install(Registry &)`, that adds its
modules to the import registry. The Series one is empty.

The two consoles differ less than the two directories suggest. There are
three kinds of console module:

- ERA / XDK only, which is Xbox One only: `kernelx.dll`, `d3d11_x.dll`, the
  WinRT classes, `etwplus.dll`. These live in `runtime/durango`.
- The same interface on both consoles under two names (`xg_x.dll` /
  `xg_xs.dll`, later `d3d12_x.dll` / `d3d12_xs.dll` and the Gaming Runtime).
  These live in `runtime/modules`, with an `install_*(Registry &, name)` that
  each console calls with the name the DLL has there. Where the two consoles
  differ inside such a module, it takes the `Target`.
- Series only: `runtime/scarlett`.

`runtime/modules` so far:

- `xg.cpp`: `xg_x.dll`, which tells titles how a texture is laid out in
  memory. Installed for Xbox One only until the Series one has been checked
  against a title.

`runtime/durango` so far:

- `kernelx.cpp`: `kernelx.dll` is forwarded to the host's `kernel32.dll`, except
  for the functions that only exist on the console.
- `winrt/activation.cpp`: ERA titles are C++/CX applications that reach the
  system through WinRT classes, all of them looked up by name through one
  function of `vccorlib110.dll`. That function is ours. It answers with our own
  class, with the host's for a few namespaces that work in any process
  (`Windows.Foundation`, `Windows.Storage.Streams`...), or with a placeholder.
- `winrt/placeholder.cpp`: an object that answers every `QueryInterface`. A
  call to one of its methods prints the class, interface, method number and
  caller, and fails. That is how the next missing piece is found.
- `winrt/inert.cpp`: table of methods of console classes whose feature is
  absent (nobody signed in, no store, no party): accept the arguments, return
  zero, hand out a token for an event that is never raised.
- `winrt/storage.cpp`: connected storage (`Windows.Xbox.Storage`), the way
  ERA titles save their games. The spaces, containers and container queries
  are ours and keep everything in files through `storage.cpp` (`SaveSpace`):
  `saves/<package name>/{machine,user}/<container>/<blob>` in the per-user
  configuration folder, with every name escaped into a portable file name.
  `launcher` picks the folder and hands it over in
  `ExecuteOptions::save_directory`. Only the methods seen in titles are
  implemented; the layout of `ContainerInfo2` is a guess.
- `winrt/application.cpp`: real classes, written with WRL against the public
  Windows SDK headers: `CoreApplication`, `CoreApplicationView`, `CoreWindow`
  and `CoreDispatcher`. `CoreApplication::Run` drives the title's view on the
  calling thread, and the window is a `TitleWindow` from `backends/surface`.

### Executable

| Directory | Library | What it does |
|---|---|---|
| `runtime` | `xs_runtime` | `console_registry(Target)`: the registry of one console |
| `launcher` | `xs_launcher` | `launch_title`, the game library, `config.ini` |
| `app/ui` | `xs_ui` | GLFW + ImGui main window, native file dialog |
| `app` | `xscarlet` | CLI |

### Where new code goes

- A new way to produce pictures, sound or input on a host (Vulkan, Direct3D
  12, another audio API): `backends/<kind>/<technology>/`.
- Works the same on both consoles and is not a console DLL (a translation
  layer, file access): a shared library next to `loader`, or under `backends/`
  if it is about the host.
- A console DLL only one console has: that console's runtime.
- A console DLL both have: `runtime/modules`, installed by each console.
- Console runtimes do not include each other.

### Windows-only code

Titles are Windows programs, and today a title only runs on Windows, where
most of what it imports is handed to the real system DLLs. That is why these
are full of Windows headers and are only built there:

- `runtime/common/windows/`: running the guest, and the COM forwarder.
- `runtime/durango/winrt/` and `runtime/durango/d3d11x/`: they give the title
  host COM objects, wrapped or as they are.

The line through Direct3D is drawn at the API, not below it: `d3d11x` turns
console Direct3D 11.X calls into desktop Direct3D 11 calls on an
`ID3D11Device`, and `backends/graphics/d3d11` is where that device comes from.
A backend for another host is therefore something that provides Direct3D 11
(or 12) interfaces there, not a second copy of `d3d11x`.

### The window

`app/ui` is Dear ImGui, in four layers that only use the ones before them:

- `theme`: fonts, colors, the pixel-art wordmark and the backdrop. The only
  place with color values and font sizes.
- `widgets`: stateless pieces built on the theme (badges, key hints, the
  segmented control, the rows of a settings page, `animate`).
- `settings`: the settings window, a list of pages and one function per page.
  A new page is an entry in `pages` and a function. It asks its owner for
  anything outside itself through `Settings::Host`.
- `desktop`: the window itself: what is loaded or running, and the library,
  loading and title screens.

The library is scanned on a worker thread. `discover_library` keeps what it
found in `cache/library.json` in the configuration folder, with the size and
time of each title's manifest and executable: a title whose files did not
change is not read again. The window shows `cached_library` (the last scan,
without reading anything) the moment it opens, and replaces it when the new
scan is done. Library pictures are decoded off the window thread too.

## Target detection

`analysis::import_profile` classifies every imported DLL with the tables of
Microsoft's `xbdepends` tool. `d3d12_x.dll`, `dxcompiler_x.dll`, `xg_x.dll`
and the ERA modules `kernelx.dll`, `d3d11_x.dll` and `etwplus.dll` mean Xbox
One (`Target::durango`). The last two are not in the `xbdepends` tables, which
only cover the GDK.
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

## Threads

Nothing a title does runs on the thread that handles a window, and the other
way round:

- the window thread runs the interface and the messages of the main window and
  of the title surface inside it;
- a worker thread reads and checks a title when it is opened;
- the guest thread runs the title, which presents straight into the surface;
- with `exec` there is no main window, and the title's own window gets a
  thread of its own for its messages.

So dragging or resizing a window does not stop the title, and a title that is
busy or stuck does not freeze the window. `CoreDispatcher::ProcessEvents` has
nothing to pump for that reason.

## Input

`src/backends/input` is the one place a console runtime asks what the gamepad reads
(`input::poll()`), and it does not depend on the window or on the runtime.
Where the reading comes from is the capture method:

- `window`: the main window (or the title's own) reports its key events, and
  the bindings turn the keys that are down into buttons, triggers and stick
  directions. Works on every host.
- `direct`: the state of the keyboard and of the host's first gamepad
  (XInput) is read from the system at the moment the title asks, without any
  window or message loop involved. Windows only. It only counts while a window
  of xscarlet is in the foreground.

Bindings are Windows virtual-key codes on every host. Options > Input in the
window changes the method and the bindings, which apply at once and are saved
by `launcher/config` to `config.ini` in the per-user configuration folder.
`runtime/durango/winrt/gamepad.cpp` is the console side: one
`Windows.Xbox.Input.Gamepad`, always connected, whose raw reading is
`input::poll()`.

## Title DLLs

A DLL that sits next to the executable and is neither ours nor forwarded is
the title's own (`xcompress.dll` in the test program). It is mapped like the
executable, its imports go through the same resolver, its exception table is
registered and its entry point is called before the title's.

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

- The command line (`GetCommandLine`, `__p___argv`, `Platform::GetCmdArguments`)
  is the title's path alone, not xscarlet's.
- Static TLS: the system loader only gives TLS to what it loads itself, so
  `runtime/common/windows/static_tls.cpp` writes a small DLL with the image's
  TLS template (and one import, without which the loader skips its TLS) and
  loads it; the image's `_tls_index` is set to the index that DLL got.
- A title's own DLLs are looked for from the executable's folder up to its
  package root, the folder with the `AppxManifest.xml`.

Things that do not work yet: delay-load imports. There is no isolation of any
kind.

## Graphics

`backends/graphics/backend` names the two backends a console Direct3D module can sit on
and holds the one selected for the process (`exec --backend`):

- `d3d11`, the host's Direct3D 11. Windows only, default there.
- `vulkan`, available everywhere, default on Linux. It cannot render a title
  yet: `xs_vulkan` is a standalone test that clears an offscreen image and
  reads it back, to have a known-good starting point.

`runtime/durango/d3d11x` is `d3d11_x.dll`. With the `d3d11` backend,
`D3D11CreateDevice` creates a device on the host; with `vulkan` selected it
fails and says so.

Direct3D 11.X interfaces have the desktop interface IDs but not the desktop
layout, so the title never gets a host object:

- `wrap.cpp`: every host object is given to the title inside an object of ours
  with the console layout. One generic entry per method slot looks the method
  up in the layout of the object's kind, replaces the arguments that are our
  objects by the host's, calls the host method, and wraps what comes back.
  The same host object always gets the same wrapper.
- `layout.cpp`: the knowledge. For each kind (device, context, swap chain,
  resource, view, state...) where every method is, which host method it is,
  and which arguments are objects. Methods with float arguments, or whose
  console signature differs from the desktop one, are written out by hand
  there. Positions that are still guesses are marked as such.
- The device context is special: titles call most of its methods through
  function pointers stored inside the object, not through the vtable, so the
  context object carries that table too.
- `device.cpp`: `D3D11CreateDevice` and the way to the swap chain. The host
  device itself is created by `backends/graphics/d3d11`. The DXGI
  device, adapter and factory are the same as on the desktop and go through
  a `Forwarder` (`runtime/common/windows/com_forward`), a COM object that
  sends every method to the host except the ones it is told to answer;
  the only one of ours is `CreateSwapChainForCoreWindow`, which makes the
  swap chain on the `TitleWindow`.

- State without calls: the console headers inline some setters. The title
  writes the input layout, the vertex and pixel shaders and the topology (as a
  hardware register value) into fixed offsets of the context object and sets
  dirty bits; `layout.cpp` reads them back and applies them before each draw.

Development switches, all environment variables: `XS_D3D_TRACE=1` prints the
first call of every Direct3D method with its arguments and caller,
`XS_D3D_DEBUG=1` creates the host device with the debug layer and prints its
messages after the call that caused them, `XS_FRAME_DUMP=file.bmp` (with
`XS_FRAME_DUMP_AT=n`) writes the n-th presented frame to a file.

`runtime/modules/xg.cpp` is `xg_x.dll`, which tells titles how a texture is
laid out in memory. There is one layout here, the plain one.

The window (`xs_ui`) uses OpenGL through GLFW for its own interface and
gets everything else from `app/main.cpp` as callbacks, so it does not link
against the runtime. Opening a title reads and checks it on a worker thread while the
window shows a loading screen, then runs it on a guest thread. The title does
not draw through the interface: `backends/surface` creates a child window
(the title surface) inside the main one, the interface tells it which rectangle
is free, and the title's swap chain presents straight into it. The surface is
shown at the title's first frame, which is also when the loading screen goes
away. With `exec` there is no main window and the title gets one of its own.

## Tests

- `tests/*.cpp`: Catch2 unit tests
- `tests/corpus/generate.py`: writes small synthetic PE files used as fixtures
- `tests/guest/`: two tiny Windows programs that the runtime loads and runs
  (Windows only)
