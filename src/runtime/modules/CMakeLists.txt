# Console modules both consoles have. Each console's runtime installs them under the name
# the DLL has there.
add_library(xs_runtime_modules xg.cpp kernel.cpp memory.cpp window.cpp system.cpp placement.cpp storage.cpp gameinput.cpp
  d3d12x/device.cpp d3d12x/present.cpp
  gameruntime/runtime.cpp gameruntime/slots.cpp gameruntime/task_queue.cpp gameruntime/async.cpp
  gameruntime/user.cpp gameruntime/save.cpp gameruntime/system.cpp)
target_include_directories(xs_runtime_modules PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xs_runtime_modules PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_runtime_modules PRIVATE xs_options xs_core xs_host xs_game_config xs_runtime_common
  xs_graphics xs_input xs_surface d3d12 dxgi shlwapi onecore)
