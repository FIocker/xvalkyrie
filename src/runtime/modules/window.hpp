#pragma once

namespace xs::surface {
class TitleWindow;
}
namespace xs::runtime {
class Registry;
namespace modules {
// The window a GDK title's frames go to. Such a title makes a window of its own, which on
// the console is nothing to look at: what it presents is shown on the display, whatever
// windows it has. Here the frames go to the title surface, as an ERA title's do, and the
// title's own windows are never shown. Created the first time it is asked for.
surface::TitleWindow &title_window();
// CreateWindowEx and ShowWindow for a title's own windows
void install_window(Registry &registry);
} // namespace modules
} // namespace xs::runtime
