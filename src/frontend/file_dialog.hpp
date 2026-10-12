#pragma once
#include <filesystem>
#include <future>

struct GLFWwindow;

namespace xs {
// Shows the native "open file" dialog for title executables without blocking the caller.
// The future yields an empty path if the user cancelled and throws if no dialog could be shown.
// Call from the thread that owns `owner`.
std::future<std::filesystem::path> pick_title_file(GLFWwindow *owner);
} // namespace xs
