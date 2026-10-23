add_library(xs_audio wasapi/mute.cpp)
target_compile_definitions(xs_audio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_audio PRIVATE xs_options)
