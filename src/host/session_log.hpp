#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace xv::host {
// From now on everything written to stdout and stderr, by xvalkyrie or by a title, also
// goes to <directory>/yyyy-mm-dd_hh-mm-ss.log. Returns that file. Call once, early.
// The copying is done by a helper process, so nothing is lost however this one ends.
// Throws if it cannot be set up; the streams are untouched then.
std::filesystem::path start_session_log(const std::filesystem::path &directory);
// Windows: the helper is this executable started with helper_option and the arguments
// start_session_log gives it. Returns its exit code.
inline constexpr char helper_option[] = "--session-log-helper";
int session_log_helper(const std::vector<std::wstring> &arguments);
} // namespace xv::host
