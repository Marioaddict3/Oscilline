// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Menu, language, and wheel cue sequences.

#pragma once

#include "oscilline/asset/menu.hpp"
#include "oscilline/audio/sfx.hpp"
#include "oscilline/course/difficulty.hpp"

#include <span>

namespace oscilline {

// Up to two cues for one title-screen action. Wheel CROSS is the difficulty
// pair. CROSS on the title list or the language list is title confirm. Any
// other CROSS is menu select plus its follow. Move on the title list, the
// language list, or options is the cursor blip. Left and right on an options
// value use that blip too. Back is the wheel "Back" line; triangle on the
// title list or the language list is silent. Options plays that back cue.
// Wheel moves and the high-score cursor are sequences, not this pair.
struct MenuSfxCues {
    SfxId ids[2]{};
    int count = 0;
};

// The four spoken lines on the game wheel. That wheel is not the title list:
// the list uses the cursor blip. These lines are still addressable so a
// caller can play them in order.
enum class GameWheelItem : std::uint8_t { Disc, OwnCd, Options, Back };

[[nodiscard]] inline std::span<const SfxId> game_wheel_announcement(GameWheelItem item) {
    static constexpr SfxId kDisc[] = {
        SfxId::AnnounceDisc0, SfxId::AnnounceDisc1, SfxId::AnnounceJoin};
    static constexpr SfxId kOwnCd[] = {SfxId::AnnounceCd0, SfxId::AnnounceCd1, SfxId::AnnounceJoin};
    static constexpr SfxId kOptions[] = {SfxId::AnnounceOptions};
    static constexpr SfxId kBack[] = {SfxId::AnnounceBack};
    switch (item) {
    case GameWheelItem::Disc:
        return kDisc;
    case GameWheelItem::OwnCd:
        return kOwnCd;
    case GameWheelItem::Options:
        return kOptions;
    case GameWheelItem::Back:
        return kBack;
    }
    return {};
}

// Difficulty-wheel rows: bronze, silver, gold, high scores, back.
[[nodiscard]] inline std::span<const SfxId> difficulty_wheel_announcement(int index) {
    static constexpr SfxId kBronze[] = {SfxId::AnnounceBronze};
    static constexpr SfxId kSilver[] = {SfxId::AnnounceSilver};
    static constexpr SfxId kGold[] = {SfxId::AnnounceGold};
    static constexpr SfxId kScores[] = {SfxId::AnnounceScores};
    static constexpr SfxId kBack[] = {SfxId::AnnounceBack};
    switch (index) {
    case 0:
        return kBronze;
    case 1:
        return kSilver;
    case 2:
        return kGold;
    case 3:
        return kScores;
    case 4:
        return kBack;
    default:
        return {};
    }
}

[[nodiscard]] inline MenuSfxCues menu_sfx_cues(MenuPage page, MenuAction action) {
    MenuSfxCues cues;
    if (action == MenuAction::Move) {
        if (page == MenuPage::Main || page == MenuPage::Language || page == MenuPage::Options) {
            cues.ids[0] = SfxId::MenuMove;
            cues.count = 1;
        }
    } else if (action == MenuAction::Adjust) {
        // Left and right on an options value use the same blip as a list move.
        if (page == MenuPage::Options) {
            cues.ids[0] = SfxId::MenuMove;
            cues.count = 1;
        }
    } else if (action == MenuAction::Back) {
        // Escape plays what confirming the page's Back row plays: the wheel's
        // difficulty confirm, the title confirm on the language list, and menu
        // select elsewhere. Escape on the title list quits, silently.
        if (page != MenuPage::Main) {
            return menu_sfx_cues(page, MenuAction::Confirm);
        }
    } else if (action == MenuAction::Confirm) {
        if (page == MenuPage::Wheel) {
            cues.ids[0] = SfxId::DifficultyConfirm;
            cues.ids[1] = SfxId::DifficultyConfirmFollow;
            cues.count = 2;
        } else if (page == MenuPage::Main || page == MenuPage::Language) {
            cues.ids[0] = SfxId::TitleConfirm;
            cues.count = 1;
        } else {
            cues.ids[0] = SfxId::MenuSelect;
            cues.ids[1] = SfxId::MenuSelectFollow;
            cues.count = 2;
        }
    }
    return cues;
}

// Sequential line for a move that landed on `index`. Empty when the move is
// the cursor blip, an edge, or not a wheel landing.
[[nodiscard]] inline std::span<const SfxId>
menu_move_sequence(MenuPage page, MenuAction action, int index) {
    if (action != MenuAction::Move) {
        return {};
    }
    if (page == MenuPage::Wheel) {
        return difficulty_wheel_announcement(index);
    }
    if (page == MenuPage::Scores) {
        if (index >= 0 && index < kDifficultyCount) {
            return difficulty_wheel_announcement(index);
        }
        if (index == kDifficultyCount) {
            return difficulty_wheel_announcement(kWheelBack);
        }
    }
    return {};
}

} // namespace oscilline
