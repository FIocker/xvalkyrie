add_library(xs_gl STATIC gl.c)
target_include_directories(xs_gl SYSTEM PUBLIC ${glfw_SOURCE_DIR}/deps)
include(FetchContent)
FetchContent_Declare(stb
  URL "https://codeload.github.com/nothings/stb/tar.gz/5736b15f7ea0ffb08dd38af21067c314d6a3aae9"
  URL_HASH "SHA256=d00921d49b06af62aa6bfb97c1b136bec661dd11dd4eecbcb0da1f6da7cedb4c")
FetchContent_MakeAvailable(stb)
# The interface font ships with Dear ImGui (Roboto Medium, Apache-2.0) and is built in.
set(xs_font "${PROJECT_BINARY_DIR}/generated/app/ui/font_roboto.cpp")
add_custom_command(OUTPUT "${xs_font}"
  COMMAND ${CMAKE_COMMAND} "-DINPUT=${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" "-DOUTPUT=${xs_font}"
    -DNAME=xs_font_roboto -P "${PROJECT_SOURCE_DIR}/cmake/Embed.cmake"
  DEPENDS "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" "${PROJECT_SOURCE_DIR}/cmake/Embed.cmake" VERBATIM)
add_library(xs_ui desktop.cpp file_dialog.cpp keys.cpp logo.cpp settings.cpp theme.cpp widgets.cpp "${xs_font}")
target_include_directories(xs_ui SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(xs_ui PRIVATE GLFW_INCLUDE_NONE)
target_compile_definitions(xs_ui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_ui PRIVATE xs_launcher xs_graphics xs_input xs_options xs_version xs_imgui xs_gl glfw nfd Threads::Threads)
