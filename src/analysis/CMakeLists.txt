add_library(xs_analysis assessment.cpp modules.cpp)
target_link_libraries(xs_analysis PUBLIC nlohmann_json::nlohmann_json PRIVATE xs_options)
