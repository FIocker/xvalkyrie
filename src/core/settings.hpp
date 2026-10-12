#pragma once
#include <iosfwd>

namespace xs {
struct Settings {
    bool vsync = true;
};
Settings read_settings(std::istream &input);
} // namespace xs
