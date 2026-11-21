add_library(xv_loader image.cpp)
target_link_libraries(xv_loader PUBLIC nlohmann_json::nlohmann_json xv_host PRIVATE xv_options xv_pe)
