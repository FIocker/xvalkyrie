#pragma once
#include <string_view>

namespace xv {
// Console generations by their Microsoft code names.
enum class Target {
    durango, // Xbox One, One S, One X
    scarlett // Xbox Series X|S
};
constexpr std::string_view target_id(Target target) {
    return target == Target::durango ? "durango" : "scarlett";
}
constexpr std::string_view target_name(Target target) {
    return target == Target::durango ? "Xbox One" : "Xbox Series X|S";
}
} // namespace xv
