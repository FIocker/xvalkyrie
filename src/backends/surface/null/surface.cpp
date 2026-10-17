#include "backends/surface/surface.hpp"

// No title surface on this host: nothing can run a title here.
namespace xs::surface {
void set_title_parent(void *) {}
void set_title_area(int, int, int, int) {}
bool title_window_shown() { return false; }
} // namespace xs::surface
