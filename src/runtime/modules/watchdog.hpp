#pragma once

namespace xs::runtime::modules {
// XS_WATCHDOG: called at each present; dumps every thread's stack once frames stop for 5 s.
// The first call starts the count, so a title that never presents is seen as well.
void frame_watchdog();
} // namespace xs::runtime::modules
