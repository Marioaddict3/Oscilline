// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Rebindable gameplay controls.

#include "oscilline/controls.hpp"

#include <cctype>
#include <cstddef>
#include <utility>

namespace oscilline {
namespace {

struct PadName {
    const char* label;
    const char* token;
};

constexpr PadName kPadNames[static_cast<std::size_t>(PadButton::Count)] = {
    {"CROSS", "south"},
    {"CIRCLE", "east"},
    {"SQUARE", "west"},
    {"TRIANGLE", "north"},
    {"SELECT", "back"},
    {"GUIDE", "guide"},
    {"START", "start"},
    {"L3", "leftstick"},
    {"R3", "rightstick"},
    {"L1", "leftshoulder"},
    {"R1", "rightshoulder"},
    {"UP", "dpup"},
    {"DOWN", "dpdown"},
    {"LEFT", "dpleft"},
    {"RIGHT", "dpright"},
};

constexpr const char* kActionLabels[kGameActionCount] = {"BLOCK", "LOOP", "WAVE", "PIT"};
constexpr const char* kActionTokens[kGameActionCount] = {"block", "loop", "wave", "pit"};

std::size_t action_index(GameAction action) {
    return static_cast<std::size_t>(action);
}

} // namespace

const char* game_action_label(GameAction action) {
    const std::size_t index = action_index(action);
    return index < static_cast<std::size_t>(kGameActionCount) ? kActionLabels[index] : "";
}

const char* game_action_token(GameAction action) {
    const std::size_t index = action_index(action);
    return index < static_cast<std::size_t>(kGameActionCount) ? kActionTokens[index] : "";
}

const char* pad_button_label(PadButton button) {
    const auto index = static_cast<std::size_t>(button);
    return index < static_cast<std::size_t>(PadButton::Count) ? kPadNames[index].label : "";
}

const char* pad_button_token(PadButton button) {
    const auto index = static_cast<std::size_t>(button);
    return index < static_cast<std::size_t>(PadButton::Count) ? kPadNames[index].token : "";
}

std::optional<PadButton> pad_button_from_token(std::string_view token) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(PadButton::Count); ++i) {
        if (key_names_equal(token, kPadNames[i].token)) {
            return static_cast<PadButton>(i);
        }
    }
    return std::nullopt;
}

bool key_names_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool key_reserved(std::string_view key_name) {
    return key_names_equal(key_name, "Escape") || key_names_equal(key_name, "P");
}

bool pad_reserved(PadButton button) {
    return button == PadButton::Start || button == PadButton::Back || button == PadButton::Guide ||
           button >= PadButton::Count;
}

bool bind_key(ControlBindings& bindings, GameAction action, std::string_view key_name) {
    const std::size_t index = action_index(action);
    if (index >= bindings.keys.size() || key_name.empty() || key_reserved(key_name)) {
        return false;
    }
    for (std::string& other : bindings.keys) {
        if (key_names_equal(other, key_name)) {
            other = bindings.keys[index];
        }
    }
    bindings.keys[index] = std::string(key_name);
    return true;
}

bool bind_pad(ControlBindings& bindings, GameAction action, PadButton button) {
    const std::size_t index = action_index(action);
    if (index >= bindings.pad.size() || pad_reserved(button)) {
        return false;
    }
    for (PadButton& other : bindings.pad) {
        if (other == button) {
            other = bindings.pad[index];
        }
    }
    bindings.pad[index] = button;
    return true;
}

std::string key_label(std::string_view key_name) {
    std::string out(key_name);
    for (char& letter : out) {
        letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    return out;
}

std::string control_hint_text(const ControlBindings& bindings, bool pad) {
    std::string out;
    for (int i = 0; i < kGameActionCount; ++i) {
        const auto action = static_cast<GameAction>(i);
        if (!out.empty()) {
            out += "   ";
        }
        out += pad ? std::string(pad_button_label(bindings.pad[static_cast<std::size_t>(i)]))
                   : key_label(bindings.keys[static_cast<std::size_t>(i)]);
        out += ' ';
        out += game_action_label(action);
    }
    return out;
}

} // namespace oscilline
