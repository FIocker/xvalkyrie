add_library(xs_host STATIC host.cpp session_log.cpp)
target_include_directories(xs_host PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(xs_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_host PRIVATE xs_options PUBLIC Threads::Threads)
