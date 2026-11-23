# xvboot: the process a title runs in. Nothing of the window is linked into it.
add_executable(xvboot main.cpp bootstrap.cpp)
target_link_libraries(xvboot PRIVATE xv_cli xv_graphics xv_surface xv_input xv_options xv_version xv_launcher xv_host xv_runtime)
set_target_properties(xvboot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
if(MSVC)
  # xvboot.map, next to the executable: what the xvboot.exe+0x... of a log's stacks are
  target_link_options(xvboot PRIVATE /MAP)
endif()
xv_deploy_agility(xvboot)
if(XV_DXVK)
  xv_deploy_dxvk(xvboot)
endif()
