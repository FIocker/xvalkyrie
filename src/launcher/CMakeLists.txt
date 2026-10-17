# What a title is and whether it can start, the game library and the saved configuration.
# No window and no console output: the CLI and the interface both sit on this.
add_library(xs_launcher launch.cpp config.cpp library.cpp)
target_link_libraries(xs_launcher PUBLIC xs_input PRIVATE xs_options xs_host xs_appx xs_pe xs_analysis xs_xvd xs_loader xs_runtime)
