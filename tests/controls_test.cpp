// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Rebindable gameplay controls.

#include "oscilline/controls.hpp"

#include <doctest/doctest.h>
#include <string>
#include <string_view>

using namespace oscilline;

TEST_CASE("default controls keep the original layout and hint line") {
    const ControlBindings defaults;
    CHECK(control_hint_text(defaults, false) == "Q BLOCK   E LOOP   X WAVE   S PIT");
    CHECK(control_hint_text(defaults, true) == "L1 BLOCK   R1 LOOP   CROSS WAVE   DOWN PIT");
}

TEST_CASE("binding an input another action uses swaps the two") {
    ControlBindings bindings;
    CHECK(bind_key(bindings, GameAction::Block, "e"));
    CHECK(bindings.keys[0] == "e");
    CHECK(bindings.keys[1] == "Q");
    CHECK(bind_pad(bindings, GameAction::Pit, PadButton::South));
    CHECK(bindings.pad[3] == PadButton::South);
    CHECK(bindings.pad[2] == PadButton::DpadDown);
    CHECK(key_label("Left Shift") == "LEFT SHIFT");
}

TEST_CASE("pause and cancel inputs cannot be bound") {
    ControlBindings bindings;
    CHECK_FALSE(bind_key(bindings, GameAction::Wave, "Escape"));
    CHECK_FALSE(bind_key(bindings, GameAction::Wave, "p"));
    CHECK_FALSE(bind_key(bindings, GameAction::Wave, ""));
    CHECK_FALSE(bind_pad(bindings, GameAction::Wave, PadButton::Start));
    CHECK_FALSE(bind_pad(bindings, GameAction::Wave, PadButton::Back));
    CHECK_FALSE(bind_pad(bindings, GameAction::Wave, PadButton::Guide));
    CHECK(bindings == ControlBindings{});
}

TEST_CASE("pad tokens round-trip") {
    for (int i = 0; i < static_cast<int>(PadButton::Count); ++i) {
        const auto button = static_cast<PadButton>(i);
        const auto parsed = pad_button_from_token(pad_button_token(button));
        REQUIRE(parsed);
        CHECK(parsed.value() == button);
        CHECK(std::string_view(pad_button_label(button)).size() > 0);
    }
    CHECK_FALSE(pad_button_from_token("triangle").has_value());
}
