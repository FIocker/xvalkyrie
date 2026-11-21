add_library(xv_analysis assessment.cpp modules.cpp)
target_link_libraries(xv_analysis PUBLIC nlohmann_json::nlohmann_json PRIVATE xv_options)
