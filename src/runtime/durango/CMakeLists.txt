add_library(xs_runtime_durango runtime.cpp kernelx.cpp xg.cpp winrt/activation.cpp)
target_include_directories(xs_runtime_durango PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xs_runtime_durango PRIVATE xs_options xs_runtime_common xs_graphics xs_input)
if(WIN32)
  target_sources(xs_runtime_durango PRIVATE winrt/placeholder.cpp winrt/inert.cpp winrt/application.cpp winrt/async.cpp winrt/collection.cpp winrt/gamepad.cpp
    d3d11x/device.cpp d3d11x/wrap.cpp d3d11x/layout.cpp d3d11x/frame_dump.cpp)
  target_compile_definitions(xs_runtime_durango PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(xs_runtime_durango PRIVATE runtimeobject d3d11 dxgi)
endif()
