# The commands that look at a title without running it.
add_library(xv_cli cli.cpp)
target_link_libraries(xv_cli PRIVATE xv_options xv_launcher xv_appx xv_pri xv_pe xv_host xv_game_config xv_analysis xv_xvd xv_loader xv_runtime xv_runtime_modules)
