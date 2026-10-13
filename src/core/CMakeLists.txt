add_library(xs_core settings.cpp console.cpp)
target_link_libraries(xs_core PUBLIC spdlog::spdlog PRIVATE xs_options nlohmann_json::nlohmann_json)
