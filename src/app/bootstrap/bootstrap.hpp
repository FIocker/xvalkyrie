#pragma once
#include "runtime/common/runtime.hpp"
#include <cstdint>
#include <filesystem>

// What starts a title: the one way from a path to a running guest, whoever asks for it
// (the window, through xvboot, or a person at the command line).
namespace xv::bootstrap {
// Runs the title `path` stands for (an executable, an unpacked title's folder or its
// manifest) in this process and in a window of its own, and returns its exit code. The
// saved games folder, the shader cache and the console settings of `options` are filled
// in here, from the configuration folder. RUNS THE TITLE'S CODE; no sandbox.
std::uint32_t run(const std::filesystem::path &path, runtime::ExecuteOptions options = {});
} // namespace xv::bootstrap
