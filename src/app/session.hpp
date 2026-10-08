// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Session options and the course, pair, and custom-music entry points.

#pragma once

#include "oscilline/asset/map.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/music.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace oscilline {

// Command-line choices that are not saved settings.
struct RunFlags {
    // Slots below this stay on the placeholder. High is the default.
    Confidence asset_confidence = kUsableConfidence;
    // Low-confidence sample rows play from the disc only when this is set.
    bool sfx_experimental = false;
    // Medium-confidence custom-music placement. Off keeps the shared default chart.
    bool gen_experimental = false;
};

// Everything a run needs besides which course or file. Build it with play_options().
struct PlayOptions {
    std::filesystem::path disc;
    // False keeps every disc-backed art slot on its placeholder.
    bool disc_assets = true;
    // Disc road camera; false is the built-in pose. Custom music uses it when a
    // disc is loaded.
    bool disc_camera = true;
    // Tick marks and the hit marker. Off leaves the ribbon as one plain line.
    bool ribbon_guides = false;
    // Empty uses the default language archive (English, then Japanese).
    std::string language_pak;
    PlaybackPrefs playback{};
    // Disc and menu text shake, in tenths. See kTextShakeDefault.
    int text_shake = kTextShakeDefault;
    HudPrefs hud{};
    RunFlags flags{};
};

// `effective` is the saved settings with command-line overrides applied.
[[nodiscard]] PlayOptions play_options(const Settings& effective,
                                       std::filesystem::path disc,
                                       std::string_view language_pak,
                                       const RunFlags& flags);

// Loads built-in course `course` (0-based) and plays it. Retry starts that
// course again. Returns a process status.
int run_session(Host& host, const PlayOptions& options, int course);

// Plays the two courses for `difficulty` back to back. Retry starts the pair
// again. Quit returns to the caller. `scores` keeps the best total.
int run_difficulty(Host& host,
                   const PlayOptions& options,
                   Difficulty difficulty,
                   ScoreBoard& scores);

// Charts `music` and plays it. Retry starts that chart again from the rabbit.
// The best score is stored in the user config directory. `cached` skips a
// second decode when the title already read that file. No disc, or disc assets
// off, keeps placeholder art, stand-in sounds, and the built-in camera. With a
// disc and the disc camera, the chart uses equal sections on the tier's course
// row, and gold applies the Gold shift.
int run_music(Host& host,
              const PlayOptions& options,
              const std::filesystem::path& music,
              Difficulty difficulty,
              const DecodedMusic* cached = nullptr);

class TextPainter;

// Draws LOADING and a progress bar. False when the window has closed.
// `text` null keeps SDL debug text.
bool present_loading(Host& host,
                     std::string_view stage,
                     float fraction,
                     const TextPainter* text = nullptr);

// Progress callback for decode and chart. `user` is a MusicLoadPump.
struct MusicLoadPump {
    Host* host = nullptr;
    // The disc font when one is loaded. Null keeps SDL debug text.
    const TextPainter* text = nullptr;
    std::uint64_t last_ms = 0;
    bool alive = true;
};

bool pump_music_load(void* user, float fraction, const char* stage);

} // namespace oscilline
