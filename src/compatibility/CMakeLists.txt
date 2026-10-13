add_library(xs_compat assessment.cpp modules.cpp)
target_link_libraries(xs_compat PUBLIC nlohmann_json::nlohmann_json PRIVATE xs_options)
