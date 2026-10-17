#include "backends/input/input.hpp"
#include <catch2/catch.hpp>
#include <nlohmann/json.hpp>
#include <set>

using namespace xs::input;

TEST_CASE("input bindings") {
    const auto settings = default_settings();
    CHECK(settings.capture == Capture::window);
    std::set<int> keys(settings.keys.begin(), settings.keys.end());
    CHECK(keys.size() == control_count); // every control has a key of its own
    CHECK_FALSE(keys.contains(0));

    std::set<int> down{' ', 'W', 'D', 'Z'};
    auto state = from_keys(settings, [&](int key) { return down.contains(key); });
    CHECK(state.buttons == Button::a);
    CHECK(state.left_x == 1);
    CHECK(state.left_y == 1);
    CHECK(state.right_x == 0);
    CHECK(state.left_trigger == 1);
    CHECK(state.right_trigger == 0);
    down = {'A', 'D', 0x28, 0x0d, 'H'}; // opposite directions cancel out
    state = from_keys(settings, [&](int key) { return down.contains(key); });
    CHECK(state.left_x == 0);
    CHECK(state.right_y == -1);
    CHECK(state.buttons == (Button::menu | Button::dpad_right));

    auto unbound = settings;
    unbound.keys[static_cast<std::size_t>(Control::a)] = 0;
    CHECK(from_keys(unbound, [](int) { return true; }).buttons ==
          (0xfffcu & ~std::uint32_t{Button::a}));
}

TEST_CASE("input merge") {
    GamepadState keyboard, gamepad;
    keyboard.buttons = Button::a;
    keyboard.left_x = 1;
    gamepad.buttons = Button::b;
    gamepad.left_x = -.5f;
    gamepad.right_y = -.25f;
    gamepad.left_trigger = .5f;
    const auto both = merge(keyboard, gamepad);
    CHECK(both.buttons == (Button::a | Button::b));
    CHECK(both.left_x == 1);
    CHECK(both.right_y == -.25f);
    CHECK(both.left_trigger == .5f);
}

TEST_CASE("input settings file") {
    auto settings = default_settings();
    settings.capture = Capture::direct;
    settings.host_gamepad = false;
    settings.keys[static_cast<std::size_t>(Control::b)] = 'K';
    const auto restored = from_json(to_json(settings));
    CHECK(restored.capture == Capture::direct);
    CHECK_FALSE(restored.host_gamepad);
    CHECK(restored.keys == settings.keys);
    // anything unusable keeps the default
    const auto defaults = default_settings();
    for (const auto *text : {"[]", "{}", R"({"capture": 3, "keys": []})",
                             R"({"keys": {"a": 999, "b": "x", "y": -1, "nope": 5}})"}) {
        const auto parsed = from_json(nlohmann::json::parse(text));
        CHECK(parsed.capture == Capture::window);
        CHECK(parsed.keys == defaults.keys);
    }
    CHECK(key_name('W') == "W");
    CHECK(key_name(0x20) == "Space");
    CHECK(key_name(0x71) == "F2");
    CHECK(key_name(0) == "(none)");
}

TEST_CASE("window capture") {
    configure(default_settings());
    window_focus_lost();
    CHECK(poll().buttons == 0);
    window_key(' ', true);
    window_key('S', true);
    window_key(999, true); // out of range, ignored
    CHECK(poll().buttons == Button::a);
    CHECK(poll().left_y == -1);
    window_key(' ', false);
    CHECK(poll().buttons == 0);
    window_focus_lost(); // nothing stays held when the window loses the keyboard
    CHECK(poll().left_y == 0);
    auto settings = default_settings();
    settings.keys[static_cast<std::size_t>(Control::y)] = 'S';
    settings.keys[static_cast<std::size_t>(Control::left_stick_down)] = 0;
    configure(settings);
    window_key('S', true);
    CHECK(poll().buttons == Button::y);
    window_focus_lost();
    configure(default_settings());
}
