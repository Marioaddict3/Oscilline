// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Rebindable gameplay controls. Menu navigation, pause, and cancel stay fixed
// so a binding can never lock a player out of the menus.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oscilline {

enum class GameAction : std::uint8_t { Block, Loop, Wave, Pit };
inline constexpr int kGameActionCount = 4;

// Same order as SDL_GamepadButton, so the host can cast between them.
enum class PadButton : std::uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count,
};

// Keyboard keys use SDL key names ("Q", "Space", "Left Shift"). The defaults
// match the original layout. While wave and pit keep their default keys, Space
// also waves and the Down arrow also pits, unless another action uses that key.
struct ControlBindings {
    std::array<std::string, kGameActionCount> keys{"Q", "E", "X", "S"};
    std::array<PadButton, kGameActionCount> pad{
        PadButton::LeftShoulder, PadButton::RightShoulder, PadButton::South, PadButton::DpadDown};

    bool operator==(const ControlBindings&) const = default;
};

[[nodiscard]] const char* game_action_label(GameAction action);
// Settings-file token for an action ("block").
[[nodiscard]] const char* game_action_token(GameAction action);

// PlayStation-style label shown in menus and hints ("L1", "CROSS").
[[nodiscard]] const char* pad_button_label(PadButton button);
// Settings-file token ("leftshoulder", "south").
[[nodiscard]] const char* pad_button_token(PadButton button);
[[nodiscard]] std::optional<PadButton> pad_button_from_token(std::string_view token);

// Escape cancels and P pauses. Start, Back, and Guide pause or cancel on a pad.
[[nodiscard]] bool key_reserved(std::string_view key_name);
[[nodiscard]] bool pad_reserved(PadButton button);

[[nodiscard]] bool key_names_equal(std::string_view a, std::string_view b);

// Assigns `key_name` to `action`. An action that already used it takes the old
// key, so two actions never share a key. A reserved or empty key is refused.
bool bind_key(ControlBindings& bindings, GameAction action, std::string_view key_name);
bool bind_pad(ControlBindings& bindings, GameAction action, PadButton button);

// Upper-cased key name for menus and hints.
[[nodiscard]] std::string key_label(std::string_view key_name);

// "Q BLOCK   E LOOP   X WAVE   S PIT", or the pad buttons when `pad` is set.
[[nodiscard]] std::string control_hint_text(const ControlBindings& bindings, bool pad);

} // namespace oscilline
