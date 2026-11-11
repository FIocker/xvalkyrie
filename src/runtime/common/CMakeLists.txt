add_library(xs_runtime_common runtime.cpp execute.cpp com_forward.cpp static_tls.cpp)
target_compile_definitions(xs_runtime_common PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(xs_runtime_common PUBLIC nlohmann_json::nlohmann_json PRIVATE xs_options xs_appx xs_pe xs_analysis xs_loader xs_host xs_audio)
target_include_directories(xs_runtime_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
