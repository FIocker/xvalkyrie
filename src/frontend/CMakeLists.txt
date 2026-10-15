add_library(xs_gl STATIC gl.c)
target_include_directories(xs_gl SYSTEM PUBLIC ${glfw_SOURCE_DIR}/deps)
include(FetchContent)
FetchContent_Declare(stb
  URL "https://codeload.github.com/nothings/stb/tar.gz/5736b15f7ea0ffb08dd38af21067c314d6a3aae9"
  URL_HASH "SHA256=d00921d49b06af62aa6bfb97c1b136bec661dd11dd4eecbcb0da1f6da7cedb4c")
FetchContent_MakeAvailable(stb)
add_library(xs_frontend desktop.cpp file_dialog.cpp logo.cpp)
target_include_directories(xs_frontend SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(xs_frontend PRIVATE GLFW_INCLUDE_NONE)
if(WIN32)
  target_compile_definitions(xs_frontend PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
target_link_libraries(xs_frontend PRIVATE xs_launch xs_input xs_options xs_version xs_imgui xs_gl glfw nfd Threads::Threads)
