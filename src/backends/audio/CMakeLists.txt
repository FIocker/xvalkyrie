add_library(xv_audio wasapi/mute.cpp)
target_compile_definitions(xv_audio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_audio PRIVATE xv_options)
