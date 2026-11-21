add_library(xv_core settings.cpp log.cpp)
target_link_libraries(xv_core PUBLIC spdlog::spdlog PRIVATE xv_options nlohmann_json::nlohmann_json)
