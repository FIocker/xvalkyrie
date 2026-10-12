# Public sources

What can be learned without NDA material, and what each source is good for.

| Source | Useful for | Not useful for |
|---|---|---|
| [Microsoft GDK](https://github.com/microsoft/GDK) and its [docs](https://learn.microsoft.com/en-us/gaming/gdk/) | API names and behavior shared between PC and console, the application model | Building console titles, that needs the Xbox Extensions (GDKX) |
| [Xbox-GDK-Samples](https://github.com/microsoft/Xbox-GDK-Samples) | Source of small titles, project configurations, the `xbdepends` tool | Binaries, there are none |
| [vcpkg Xbox triplets](https://learn.microsoft.com/en-us/vcpkg/users/platforms/xbox) | How libraries are built for the console (API sets, dynamic CRT) | D3D12.X, which needs GDKX |
| [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) and [x64 ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention) | Everything about the executable format and calling convention | Console specific additions, if any |

## Samples

In Xbox-GDK-Samples each project has separate configurations per platform.
`Gaming.Xbox.Scarlett.x64` is the Series one and it only builds with GDKX. The
desktop configuration of the same sample is a normal Windows program that uses
stock D3D12, so it tells us nothing about the console runtime.

SimpleTriangle is the obvious first target: it calls `XGameRuntimeInitialize`,
`D3D12XboxCreateDevice`, clears the screen and draws one triangle.

## Open questions

- Imports of a real Series build (function names, ordinals)
- How D3D12.X differs from D3D12 at the ABI level
- Memory layout, thread affinity and scheduling that titles rely on
- Storage paths and how the package is mounted
