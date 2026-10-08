// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Options-menu settings. The file sits beside the music scores.

#pragma once

#include "oscilline/controls.hpp"
#include "oscilline/render/viewport.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace oscilline {

// Click the calibration test plays, and the gap the flash follows.
inline constexpr int kCalibrationIntervalMs = 500;
inline constexpr int kCalibrationFlashMs = 80;
inline constexpr int kCalibrationTaps = 8;
// Manual left/right and a finished test both stay inside this.
inline constexpr int kTimingOffsetMinMs = -500;
inline constexpr int kTimingOffsetMaxMs = 500;
inline constexpr int kVolumePercentMax = 100;
// Window scale is based on the selected viewport aspect ratio.
inline constexpr int kWindowScaleMax = 8;
// Disc and menu text shake, in tenths of the options multiplier.
// 0 is off. 10 is 1×. 20 is 2×. That 1× is half the built-in
// disc-font amplitude, so 2× is the built-in amplitude.
inline constexpr int kTextShakeMin = 0;
inline constexpr int kTextShakeMax = 20;
inline constexpr int kTextShakeDefault = 10;

// Volumes and the judgment offset. Ribbon, camera, and the window are separate.
struct PlaybackPrefs {
    int timing_offset_ms = 0;
    int music_volume = 100;
    int sfx_volume = 100;
};

// In-level HUD choices from Options > Interface.
struct HudPrefs {
    // True draws the seven score coupons. False shows the score as a number.
    bool score_coupons = true;
    // The PERFECT, GOOD, and MISS text below the coupons. Hidden by default.
    bool timing_hints = false;
    // The gameplay control line at the top left. Hidden by default.
    bool control_hints = false;

    bool operator==(const HudPrefs&) const = default;
};

// Saved options. Defaults match a launch with no flags and no file:
// 4:3 viewport, disc camera on, ribbon guides off, full volume, no timing shift,
// score coupons, no timing or control hints, and the original controls.
struct Settings {
    bool ribbon_guides = false;
    bool disc_camera = true;
    bool fullscreen = false;
    int window_scale = 0;
    ViewportAspect aspect_ratio = ViewportAspect::FourThree;
    // Options > Video > Resolution. False is HD: the full window resolution.
    // True is Classic: the original 286-line frame, upscaled with hard pixels.
    bool classic_resolution = false;
    // See kTextShakeDefault. Left and right on the Options row step by one tenth.
    int text_shake = kTextShakeDefault;
    // True uses disc models, menu art, and SFX. False is built-in presentation.
    // The disc path can stay mounted for courses and CD audio.
    bool disc_assets = true;
    std::string disc_path;
    // Empty keeps the default archive (English, then Japanese).
    std::string language_pak;
    PlaybackPrefs playback{};
    HudPrefs hud{};
    ControlBindings controls{};
};

// Set when that flag was passed. An empty optional leaves the saved value.
struct SettingsOverrides {
    std::optional<bool> ribbon_guides;
    std::optional<bool> disc_camera;
    // --no-disc-assets forces false for this run. The saved value stays.
    std::optional<bool> disc_assets;
};

// key=value file in the user config directory. See settings_path().
// Unknown keys are ignored. A value that does not parse leaves that key's
// default (or an earlier valid copy of the same key). A missing, empty, or
// unreadable file is defaults.
struct SettingsLoad {
    Settings settings{};
    bool missing = false;
    std::string error;
};

[[nodiscard]] SettingsLoad load_settings(const std::filesystem::path& file);

// Creates parent directories. An empty path is a no-op success.
[[nodiscard]] Result<int> save_settings(const std::filesystem::path& file,
                                        const Settings& settings);

// Same directory as music-scores.txt. Empty when no home directory can be found.
[[nodiscard]] std::filesystem::path settings_path();

[[nodiscard]] Settings apply_settings_overrides(Settings settings,
                                                const SettingsOverrides& overrides);

[[nodiscard]] inline PlaybackPrefs playback_from(const Settings& settings) {
    return settings.playback;
}

// 100 -> 32767, 0 -> 0. The mixer treats 32767 as unity.
[[nodiscard]] inline int volume_percent_to_q15(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > kVolumePercentMax) {
        percent = kVolumePercentMax;
    }
    return static_cast<int>((static_cast<std::int64_t>(percent) * 32767) / kVolumePercentMax);
}

[[nodiscard]] int clamp_timing_offset(int offset_ms);

// Multiplier of the built-in disc-font amplitude. 0 is still.
// Options 1× (tenths 10) is half that amplitude. Options 2× (tenths 20)
// is that amplitude. Values outside 0–20 are clamped.
[[nodiscard]] inline float text_shake_scale(int tenths) {
    if (tenths < kTextShakeMin) {
        tenths = kTextShakeMin;
    }
    if (tenths > kTextShakeMax) {
        tenths = kTextShakeMax;
    }
    return static_cast<float>(tenths) * 0.1f * 0.5f;
}

// Nearest multiple of `interval_ms`. Halfway rounds away from zero.
// A non-positive interval returns `tap_ms`, so the offset is zero.
[[nodiscard]] std::int64_t nearest_click_ms(std::int64_t tap_ms, int interval_ms);

// Mean of (tap − click), rounded to nearest and clamped. Empty is 0.
[[nodiscard]] int average_timing_offset(std::span<const int> tap_minus_click_ms);

} // namespace oscilline
