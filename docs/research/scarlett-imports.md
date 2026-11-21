# What a Series executable looks like

Notes from reading [xbdepends](https://github.com/microsoft/Xbox-GDK-Samples/tree/b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Samples/Tools/xbdepends),
a small tool Microsoft ships in the public GDK samples to diagnose import
problems in console titles. It is MIT licensed and it is the best public
description of what a title links against.

## Findings

- A Series title is a regular PE32+ x64 executable. The sample output in the
  tool's [readme](https://learn.microsoft.com/en-us/samples/microsoft/xbox-gdk-samples/xbdepends/)
  shows subsystem 2 (Windows GUI), not the Xbox subsystem value.
- The Direct3D DLL tells you the target:

  | DLLs | Target |
  |---|---|
  | `d3d12_xs.dll`, `dxcompiler_xs.dll`, `xg_xs.dll` | Series X\|S |
  | `d3d12_x.dll`, `dxcompiler_x.dll`, `xg_x.dll` | Xbox One |
  | `d3d12.dll`, `d3d11.dll`, `dxgi.dll`, ... | PC |

- The system API comes from API sets (`api-ms-win-*`, `ext-ms-win-*`). The
  template project in the readme imports 27 DLLs: 22 API sets, `d3d12_xs.dll`,
  `PIXEvt.dll` and 3 CRT DLLs.
- DLLs that only exist on the console: `AcpHal.dll`, `chad.dll`, `pixevt.dll`,
  `xapu.dll`, `xfrontpaneldisplay.dll`, `xmem.dll`.
- `kernelx.dll` belongs to the old Xbox One (ERA) model and is not supported
  by the GDK.
- `GameOSAPIs.h` lists 1487 OS functions titles are allowed to import.
- `xgameruntime.dll` does not show up anywhere. The Gaming Runtime is linked
  statically; how it talks to the system at run time is not covered here.

## How XValkyrie uses it

`tools/import_xbdepends.py` downloads the three source files at a pinned
commit, checks their hashes and generates `src/analysis/xbdepends_data.hpp`.
`xvalkyrie assess` and `xvalkyrie bind` use those tables, and the runtime uses
the API list to decide which calls may be forwarded to Windows.

One difference: the original tool searches its patterns anywhere in the DLL
name and ignores case for API names. We match whole names and exact case.

## What a real build showed

A Series program built from the public samples, and a retail Xbox One GDK title, are
described in [gdk-titles.md](gdk-titles.md): the imports, how `xgameruntime.dll` is
reached, and what `d3d12_xs.dll` has to be.
