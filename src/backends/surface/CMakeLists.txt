# The window a title presents into.
add_library(xv_surface win32/window.cpp win32/prompt.cpp win32/icon.cpp)
target_compile_definitions(xv_surface PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_surface PRIVATE xv_options xv_input gdiplus Threads::Threads)
