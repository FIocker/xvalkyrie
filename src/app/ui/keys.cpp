#include "app/ui/keys.hpp"
#include <GLFW/glfw3.h>

namespace xv::ui {
int virtual_key(int key) {
    if ((key >= GLFW_KEY_0 && key <= GLFW_KEY_9) || (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) ||
        key == GLFW_KEY_SPACE)
        return key;
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
        return 0x70 + key - GLFW_KEY_F1;
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
        return 0x60 + key - GLFW_KEY_KP_0;
    switch (key) {
    case GLFW_KEY_ENTER:
        return 0x0d;
    case GLFW_KEY_TAB:
        return 0x09;
    case GLFW_KEY_BACKSPACE:
        return 0x08;
    case GLFW_KEY_ESCAPE:
        return 0x1b;
    case GLFW_KEY_LEFT:
        return 0x25;
    case GLFW_KEY_UP:
        return 0x26;
    case GLFW_KEY_RIGHT:
        return 0x27;
    case GLFW_KEY_DOWN:
        return 0x28;
    case GLFW_KEY_INSERT:
        return 0x2d;
    case GLFW_KEY_DELETE:
        return 0x2e;
    case GLFW_KEY_PAGE_UP:
        return 0x21;
    case GLFW_KEY_PAGE_DOWN:
        return 0x22;
    case GLFW_KEY_HOME:
        return 0x24;
    case GLFW_KEY_END:
        return 0x23;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
        return 0x10;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL:
        return 0x11;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT:
        return 0x12;
    case GLFW_KEY_SEMICOLON:
        return 0xba;
    case GLFW_KEY_EQUAL:
        return 0xbb;
    case GLFW_KEY_COMMA:
        return 0xbc;
    case GLFW_KEY_MINUS:
        return 0xbd;
    case GLFW_KEY_PERIOD:
        return 0xbe;
    case GLFW_KEY_SLASH:
        return 0xbf;
    case GLFW_KEY_GRAVE_ACCENT:
        return 0xc0;
    case GLFW_KEY_LEFT_BRACKET:
        return 0xdb;
    case GLFW_KEY_BACKSLASH:
        return 0xdc;
    case GLFW_KEY_RIGHT_BRACKET:
        return 0xdd;
    case GLFW_KEY_APOSTROPHE:
        return 0xde;
    default:
        return 0;
    }
}
} // namespace xv::ui
