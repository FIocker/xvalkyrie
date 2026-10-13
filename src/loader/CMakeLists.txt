add_library(xs_loader image.cpp)
target_link_libraries(xs_loader PUBLIC nlohmann_json::nlohmann_json xs_host PRIVATE xs_options xs_pe)
