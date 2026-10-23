add_library(xs_input input.cpp xinput/direct.cpp)
target_compile_definitions(xs_input PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(xs_input PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xs_input PRIVATE xs_options xinput9_1_0)
