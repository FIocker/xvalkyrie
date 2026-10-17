# Console modules both consoles have. Each console's runtime installs them under the name
# the DLL has there.
add_library(xs_runtime_modules xg.cpp)
target_link_libraries(xs_runtime_modules PRIVATE xs_options xs_runtime_common)
