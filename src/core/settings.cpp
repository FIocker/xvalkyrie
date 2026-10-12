#include "core/settings.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace xs {
Settings read_settings(std::istream &input) {
    const auto doc = nlohmann::json::parse(input);
    if (!doc.is_object()) {
        throw std::runtime_error("Settings must be a JSON object");
    }
    Settings result;
    for (const auto &[key, value] : doc.items()) {
        if (key != "vsync" || !value.is_boolean()) {
            throw std::runtime_error("Invalid setting: " + key);
        }
        result.vsync = value.get<bool>();
    }
    return result;
}
} // namespace xs
