include(ExternalProject)
find_package(Git REQUIRED)
find_program(XS_MESON meson REQUIRED)
find_program(XS_GLSLANG NAMES glslang glslangValidator REQUIRED)

set(xs_dxvk_commit 2c94c5f25cf75c313ef2a55b2e0c9276d67c28d6)
set(xs_dxvk_build "${PROJECT_BINARY_DIR}/dxvk/build")
set(xs_dxvk_native "${PROJECT_BINARY_DIR}/dxvk/native.ini")
file(WRITE "${xs_dxvk_native}" "[binaries]\nglslang = '${XS_GLSLANG}'\n")

ExternalProject_Add(xs_dxvk
  GIT_REPOSITORY https://github.com/doitsujin/dxvk.git
  GIT_TAG ${xs_dxvk_commit}
  GIT_SUBMODULES_RECURSE ON
  GIT_PROGRESS OFF
  UPDATE_DISCONNECTED ON
  PREFIX "${PROJECT_BINARY_DIR}/dxvk"
  BINARY_DIR "${xs_dxvk_build}"
  PATCH_COMMAND ${GIT_EXECUTABLE} checkout -- . COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn "${PROJECT_SOURCE_DIR}/cmake/dxvk.patch"
  CONFIGURE_COMMAND ${XS_MESON} setup --reconfigure --vsenv --buildtype=release --native-file "${xs_dxvk_native}"
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false <BINARY_DIR> <SOURCE_DIR>
  BUILD_COMMAND ${XS_MESON} compile -C <BINARY_DIR>
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${xs_dxvk_build}/src/d3d11/d3d11.dll" "${xs_dxvk_build}/src/dxgi/dxgi.dll")

function(xs_deploy_dxvk target)
  add_dependencies(${target} xs_dxvk)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${xs_dxvk_build}/src/d3d11/d3d11.dll" "$<TARGET_FILE_DIR:${target}>/xs_d3d11.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${xs_dxvk_build}/src/dxgi/dxgi.dll" "$<TARGET_FILE_DIR:${target}>/xs_dxgi.dll"
    VERBATIM)
endfunction()
