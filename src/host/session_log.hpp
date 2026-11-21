#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace xv::host {
// For a program without a console of its own (a window program): stdout and stderr that
// lead nowhere go to the console of the program that started this one, if it has one.
// Call before start_session_log, which copies to where the streams lead.
void use_parent_console();
// From now on everything written to stdout and stderr, by this program or by a title, also
// goes to <directory>/yyyy-mm-dd_hh-mm-ss.log (with _2, _3... if taken). Returns that file. Call
// once, early. The copying is done by a helper process, so nothing is lost however this one ends.
// Throws if it cannot be set up; the streams are untouched then.
std::filesystem::path start_session_log(const std::filesystem::path &directory);
// Windows: the helper is this executable started with helper_option and the arguments
// start_session_log gives it. Returns its exit code.
inline constexpr char helper_option[] = "--session-log-helper";
int session_log_helper(const std::vector<std::wstring> &arguments);
} // namespace xv::host
