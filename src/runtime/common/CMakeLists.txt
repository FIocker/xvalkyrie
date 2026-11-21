add_library(xv_runtime_common runtime.cpp execute.cpp com_forward.cpp static_tls.cpp)
target_compile_definitions(xv_runtime_common PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xv_runtime_common PUBLIC nlohmann_json::nlohmann_json PRIVATE xv_options xv_appx xv_pe xv_analysis xv_loader xv_host xv_audio)
target_include_directories(xv_runtime_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
