include(ExternalProject)
find_package(Git REQUIRED)
find_program(XV_MESON meson REQUIRED)
find_program(XV_GLSLANG NAMES glslang glslangValidator REQUIRED)

set(xv_dxvk_commit 2c94c5f25cf75c313ef2a55b2e0c9276d67c28d6)
set(xv_dxvk_build "${PROJECT_BINARY_DIR}/dxvk/build")
set(xv_dxvk_native "${PROJECT_BINARY_DIR}/dxvk/native.ini")
file(WRITE "${xv_dxvk_native}" "[binaries]\nglslang = '${XV_GLSLANG}'\n")

ExternalProject_Add(xv_dxvk
  GIT_REPOSITORY https://github.com/doitsujin/dxvk.git
  GIT_TAG ${xv_dxvk_commit}
  GIT_SUBMODULES_RECURSE ON
  GIT_PROGRESS OFF
  UPDATE_DISCONNECTED ON
  PREFIX "${PROJECT_BINARY_DIR}/dxvk"
  BINARY_DIR "${xv_dxvk_build}"
  PATCH_COMMAND ${GIT_EXECUTABLE} checkout -- . COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn "${PROJECT_SOURCE_DIR}/cmake/dxvk.patch"
  CONFIGURE_COMMAND ${XV_MESON} setup --reconfigure --vsenv --buildtype=release --native-file "${xv_dxvk_native}"
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false <BINARY_DIR> <SOURCE_DIR>
  BUILD_COMMAND ${XV_MESON} compile -C <BINARY_DIR>
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${xv_dxvk_build}/src/d3d11/d3d11.dll" "${xv_dxvk_build}/src/dxgi/dxgi.dll")

function(xv_deploy_dxvk target)
  add_dependencies(${target} xv_dxvk)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${xv_dxvk_build}/src/d3d11/d3d11.dll" "$<TARGET_FILE_DIR:${target}>/xv_d3d11.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${xv_dxvk_build}/src/dxgi/dxgi.dll" "$<TARGET_FILE_DIR:${target}>/xv_dxgi.dll"
    VERBATIM)
endfunction()
