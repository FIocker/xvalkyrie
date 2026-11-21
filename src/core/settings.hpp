#pragma once
#include <iosfwd>

namespace xv {
struct Settings {
    bool vsync = true;
};
Settings read_settings(std::istream &input);
} // namespace xv
