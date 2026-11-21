add_library(xv_input input.cpp xinput/direct.cpp)
target_compile_definitions(xv_input PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(xv_input PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xv_input PRIVATE xv_options xinput9_1_0)
