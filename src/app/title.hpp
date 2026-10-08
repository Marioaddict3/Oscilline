// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Title-screen entry. Play opens the difficulty wheel.

#pragma once

#include "oscilline/asset/map.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/settings.hpp"
#include "session.hpp"

#include <filesystem>
#include <string_view>

namespace oscilline {

// Where the title starts. `open_wheel` 0, 1, or 2 starts on that wheel row.
// `open_music` 0, 1, or 2 starts on that row of the music picker when `music`
// is set. Any other value opens the main menu.
struct TitleStart {
    std::filesystem::path disc;
    std::filesystem::path music;
    int open_wheel = -1;
    int open_music = -1;
};

// Title screen. Play Original opens the difficulty wheel when a disc is loaded
// and disc assets are on. Options edits `effective` and writes `stored` (the
// file copy, without command-line overrides). `disc_assets_locked` is
// `--no-disc-assets`: the session stays on built-in presentation and the saved
// flag is left alone. Returns 0 when the window closes or the user quits.
int run_title(Host& host,
              TitleStart start,
              Settings effective,
              Settings stored,
              std::string_view language_pak,
              const RunFlags& flags,
              bool disc_assets_locked = false);

} // namespace oscilline
