add_library(xs_core settings.cpp log.cpp)
target_link_libraries(xs_core PUBLIC spdlog::spdlog PRIVATE xs_options nlohmann_json::nlohmann_json)
