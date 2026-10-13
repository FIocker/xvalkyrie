add_subdirectory(common)
add_subdirectory(durango)
add_subdirectory(scarlett)
add_library(xs_runtime console.cpp)
target_link_libraries(xs_runtime PUBLIC xs_runtime_common
  PRIVATE xs_options xs_pe xs_compat xs_runtime_durango xs_runtime_scarlett)
