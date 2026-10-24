# The window a title presents into.
add_library(xs_surface win32/window.cpp win32/prompt.cpp)
target_compile_definitions(xs_surface PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_surface PRIVATE xs_options xs_input Threads::Threads)
