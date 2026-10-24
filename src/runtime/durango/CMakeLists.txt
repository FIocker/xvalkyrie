add_library(xs_runtime_durango runtime.cpp kernelx.cpp memory.cpp system.cpp storage.cpp winrt/activation.cpp
  winrt/placeholder.cpp winrt/inert.cpp winrt/application.cpp winrt/async.cpp winrt/collection.cpp winrt/gamepad.cpp winrt/storage.cpp winrt/system_ui.cpp winrt/title_info.cpp
  d3d11x/device.cpp d3d11x/wrap.cpp d3d11x/layout.cpp)
target_include_directories(xs_runtime_durango PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xs_runtime_durango PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_runtime_durango PRIVATE xs_options xs_appx xs_host xs_runtime_common xs_runtime_modules xs_graphics xs_input xs_surface
  xs_graphics_d3d11 xs_graphics_dxvk runtimeobject d3d11 dxgi shlwapi onecore)
