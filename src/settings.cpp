// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// key=value load and save for the options menu.

#include "oscilline/settings.hpp"

#include "oscilline/asset/menu.hpp"
#include "oscilline/course/music.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oscilline {
namespace {

constexpr int kMaxLine = 4096;

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string lower_copy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::optional<bool> parse_bool(std::string_view text) {
    const std::string value = lower_copy(trim(text));
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<int> parse_int(std::string_view text, int min_value, int max_value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return std::nullopt;
    }
    int parsed = 0;
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    if (parsed < min_value || parsed > max_value) {
        return std::nullopt;
    }
    return parsed;
}

void apply_line(Settings& settings, std::string_view line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
        return;
    }
    const auto split = trimmed.find('=');
    if (split == std::string::npos) {
        return;
    }
    const std::string key = lower_copy(trim(std::string_view(trimmed).substr(0, split)));
    const std::string_view value(trimmed.data() + split + 1, trimmed.size() - split - 1);
    if (key == "ribbon_guides") {
        if (const auto parsed = parse_bool(value)) {
            settings.ribbon_guides = parsed.value();
        }
    } else if (key == "disc_camera") {
        if (const auto parsed = parse_bool(value)) {
            settings.disc_camera = parsed.value();
        }
    } else if (key == "aspect_ratio") {
        const std::string token = lower_copy(trim(value));
        if (token == "4:3") {
            settings.aspect_ratio = ViewportAspect::FourThree;
        } else if (token == "16:9") {
            settings.aspect_ratio = ViewportAspect::SixteenNine;
        } else if (token == "unrestricted") {
            settings.aspect_ratio = ViewportAspect::Unrestricted;
        }
    } else if (key == "resolution") {
        const std::string token = lower_copy(trim(value));
        if (token == "hd") {
            settings.classic_resolution = false;
        } else if (token == "classic") {
            settings.classic_resolution = true;
        }
    } else if (key == "fullscreen") {
        if (const auto parsed = parse_bool(value)) {
            settings.fullscreen = parsed.value();
        }
    } else if (key == "music_volume") {
        if (const auto parsed = parse_int(value, 0, kVolumePercentMax)) {
            settings.playback.music_volume = parsed.value();
        }
    } else if (key == "sfx_volume") {
        if (const auto parsed = parse_int(value, 0, kVolumePercentMax)) {
            settings.playback.sfx_volume = parsed.value();
        }
    } else if (key == "timing_offset_ms") {
        if (const auto parsed = parse_int(value, kTimingOffsetMinMs, kTimingOffsetMaxMs)) {
            settings.playback.timing_offset_ms = parsed.value();
        }
    } else if (key == "window_scale") {
        if (const auto parsed = parse_int(value, 0, kWindowScaleMax)) {
            settings.window_scale = parsed.value();
        }
    } else if (key == "text_shake") {
        if (const auto parsed = parse_int(value, kTextShakeMin, kTextShakeMax)) {
            settings.text_shake = parsed.value();
        }
    } else if (key == "disc_assets") {
        if (const auto parsed = parse_bool(value)) {
            settings.disc_assets = parsed.value();
        }
    } else if (key == "score_display") {
        const std::string token = lower_copy(trim(value));
        if (token == "coupons") {
            settings.hud.score_coupons = true;
        } else if (token == "number") {
            settings.hud.score_coupons = false;
        }
    } else if (key == "timing_hints") {
        if (const auto parsed = parse_bool(value)) {
            settings.hud.timing_hints = parsed.value();
        }
    } else if (key == "control_hints" || key == "controller_hints") {
        // controller_hints is the older name, still read from existing files.
        if (const auto parsed = parse_bool(value)) {
            settings.hud.control_hints = parsed.value();
        }
    } else if (key.starts_with("key_") || key.starts_with("pad_")) {
        for (int i = 0; i < kGameActionCount; ++i) {
            const auto action = static_cast<GameAction>(i);
            if (key.substr(4) != game_action_token(action)) {
                continue;
            }
            // Through bind_*, so a duplicate swaps rather than leaving two actions on one input.
            if (key[0] == 'k') {
                (void)bind_key(settings.controls, action, trim(value));
            } else if (const auto button = pad_button_from_token(trim(value))) {
                (void)bind_pad(settings.controls, action, button.value());
            }
        }
    } else if (key == "disc_path") {
        settings.disc_path = trim(value);
    } else if (key == "language") {
        const std::string token = trim(value);
        if (token.empty()) {
            return;
        }
        if (const auto named = language_pak_for(token)) {
            settings.language_pak = named.value();
            return;
        }
        for (const LanguageRow& row : language_rows()) {
            if (token == row.pak) {
                settings.language_pak = token;
                return;
            }
        }
    }
}

const char* bool_text(bool value) {
    return value ? "true" : "false";
}

const char* aspect_ratio_value(ViewportAspect aspect) {
    switch (aspect) {
    case ViewportAspect::FourThree:
        return "4:3";
    case ViewportAspect::SixteenNine:
        return "16:9";
    case ViewportAspect::Unrestricted:
        return "unrestricted";
    }
    return "4:3";
}

} // namespace

int clamp_timing_offset(int offset_ms) {
    if (offset_ms < kTimingOffsetMinMs) {
        return kTimingOffsetMinMs;
    }
    if (offset_ms > kTimingOffsetMaxMs) {
        return kTimingOffsetMaxMs;
    }
    return offset_ms;
}

std::int64_t nearest_click_ms(std::int64_t tap_ms, int interval_ms) {
    if (interval_ms <= 0) {
        return tap_ms;
    }
    const auto interval = static_cast<std::int64_t>(interval_ms);
    if (tap_ms >= 0) {
        return ((tap_ms + interval / 2) / interval) * interval;
    }
    const std::int64_t flipped = ((-tap_ms + interval / 2) / interval) * interval;
    return -flipped;
}

int average_timing_offset(std::span<const int> tap_minus_click_ms) {
    if (tap_minus_click_ms.empty()) {
        return 0;
    }
    std::int64_t sum = 0;
    for (const int sample : tap_minus_click_ms) {
        sum += sample;
    }
    const auto count = static_cast<std::int64_t>(tap_minus_click_ms.size());
    int mean = 0;
    if (sum >= 0) {
        mean = static_cast<int>((sum + count / 2) / count);
    } else {
        mean = static_cast<int>(-((-sum + count / 2) / count));
    }
    return clamp_timing_offset(mean);
}

SettingsLoad load_settings(const std::filesystem::path& file) {
    SettingsLoad loaded;
    if (file.empty()) {
        loaded.missing = true;
        return loaded;
    }
    std::error_code error;
    if (!std::filesystem::exists(file, error) || error) {
        loaded.missing = true;
        return loaded;
    }
    std::ifstream in(file);
    if (!in) {
        loaded.error = "could not read settings";
        return loaded;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() > static_cast<std::size_t>(kMaxLine)) {
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        apply_line(loaded.settings, line);
    }
    if (in.bad()) {
        loaded = {};
        loaded.error = "could not read settings";
    }
    return loaded;
}

Result<int> save_settings(const std::filesystem::path& file, const Settings& settings) {
    if (file.empty()) {
        return Result<int>::success(0);
    }
    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            return Result<int>::failure("could not save settings");
        }
    }
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return Result<int>::failure("could not save settings");
    }
    const char* const aspect_value = aspect_ratio_value(settings.aspect_ratio);
    out << "# Oscilline settings. One key=value per line.\n"
        << "# Unknown keys are ignored. A bad value keeps that key's default.\n"
        << "# ribbon_guides: true draws ribbon ticks, the hit marker, and hit windows.\n"
        << "# music_volume and sfx_volume: 0 to 100.\n"
        << "# timing_offset_ms: milliseconds subtracted from a press. 0 is no shift.\n"
        << "# disc_camera: true is the disc camera, false is the built-in camera.\n"
        << "# fullscreen: true or false.\n"
        << "# aspect_ratio: 4:3, 16:9, or unrestricted.\n"
        << "# window_scale: 0 keeps the default window size; positive values scale it.\n"
        << "# text_shake: 0 is off, 10 is 1x (half the built-in disc-font shake), 20 is 2x "
           "(that shake).\n"
        << "# disc_assets: false keeps built-in graphics, camera, and sounds.\n"
        << "# language: english, japanese, german, spanish, french, or italian.\n"
        << "# score_display: coupons or number.\n"
        << "# controller_hints: true shows the control line during play.\n"
        << "# key_<action>: SDL key name. pad_<action>: south, east, west, north, leftshoulder,\n"
        << "#   rightshoulder, leftstick, rightstick, dpup, dpdown, dpleft, or dpright.\n"
        << "# disc_path: last selected cue or disc image path.\n"
        << "ribbon_guides=" << bool_text(settings.ribbon_guides) << '\n'
        << "music_volume=" << settings.playback.music_volume << '\n'
        << "sfx_volume=" << settings.playback.sfx_volume << '\n'
        << "timing_offset_ms=" << settings.playback.timing_offset_ms << '\n'
        << "disc_camera=" << bool_text(settings.disc_camera) << '\n'
        << "fullscreen=" << bool_text(settings.fullscreen) << '\n'
        << "aspect_ratio=" << aspect_value << '\n'
        << "window_scale=" << settings.window_scale << '\n'
        << "resolution=" << (settings.classic_resolution ? "classic" : "hd") << '\n'
        << "text_shake=" << settings.text_shake << '\n'
        << "disc_assets=" << bool_text(settings.disc_assets) << '\n'
        << "language=" << settings.language_pak << '\n'
        << "score_display=" << (settings.hud.score_coupons ? "coupons" : "number") << '\n'
        << "timing_hints=" << bool_text(settings.hud.timing_hints) << '\n'
        << "control_hints=" << bool_text(settings.hud.control_hints) << '\n';
    for (int i = 0; i < kGameActionCount; ++i) {
        const auto action = static_cast<GameAction>(i);
        const auto index = static_cast<std::size_t>(i);
        out << "key_" << game_action_token(action) << '=' << settings.controls.keys[index] << '\n'
            << "pad_" << game_action_token(action) << '='
            << pad_button_token(settings.controls.pad[index]) << '\n';
    }
    out << "disc_path=" << settings.disc_path << '\n';
    if (!out) {
        return Result<int>::failure("could not save settings");
    }
    return Result<int>::success(0);
}

std::filesystem::path settings_path() {
    const std::filesystem::path dir = user_config_directory();
    if (dir.empty()) {
        return {};
    }
    return dir / "settings.txt";
}

Settings apply_settings_overrides(Settings settings, const SettingsOverrides& overrides) {
    if (overrides.ribbon_guides) {
        settings.ribbon_guides = overrides.ribbon_guides.value();
    }
    if (overrides.disc_camera) {
        settings.disc_camera = overrides.disc_camera.value();
    }
    if (overrides.disc_assets) {
        settings.disc_assets = overrides.disc_assets.value();
    }
    return settings;
}

} // namespace oscilline
