# The window a title presents into.
if(WIN32)
  add_library(xs_surface win32/window.cpp)
  target_compile_definitions(xs_surface PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  add_library(xs_surface null/surface.cpp)
endif()
target_link_libraries(xs_surface PRIVATE xs_options xs_input Threads::Threads)
