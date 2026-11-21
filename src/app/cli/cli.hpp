#pragma once
#include <optional>
#include <string_view>

namespace xv::cli {
// The commands that look at a title without running it (inspect, assess, map, bind...),
// one line each, for the usage text.
extern const std::string_view commands;
// Runs the command argv[1] names and returns its exit code; nothing if it names none of
// them, or not with these arguments. Prints JSON on stdout. May throw.
std::optional<int> run(int argc, wchar_t **argv);
} // namespace xv::cli
