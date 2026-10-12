# Vulkan clear test

`xscarlet-vk-check` (built with `-DXS_VULKAN=ON`) clears an offscreen RGBA8
image, clears a few rectangles on top with `vkCmdClearAttachments`, reads the
image back and compares every pixel with values computed on the CPU.

It exists to have a small, verified piece of Vulkan code to grow the renderer
from. It mirrors what the SimpleTriangle sample does first
(`ClearRenderTargetView` on the back buffer), but nothing from a guest is
connected to it.

Not covered: sRGB, MSAA, depth/stencil, shaders, presentation, or anything
specific to D3D12.X.

There is no swapchain and no window involved, so it runs on headless machines
with a software driver like lavapipe.
