add_subdirectory(common)
add_subdirectory(modules)
add_subdirectory(durango)
add_subdirectory(scarlett)
add_library(xv_runtime console.cpp)
target_link_libraries(xv_runtime PUBLIC xv_runtime_common
  PRIVATE xv_options xv_pe xv_analysis xv_runtime_durango xv_runtime_scarlett)
