// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Settings file, command-line override, and the calibration offset.

#include "oscilline/asset/menu.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/settings.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_settings(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "oscilline-settings-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::filesystem::remove(file);
    return file;
}

void write_file(const std::filesystem::path& file, const std::string& body) {
    std::ofstream out(file, std::ios::trunc);
    REQUIRE(out);
    out << body;
}

oscilline::CourseTimeline centered_block() {
    oscilline::CourseTimeline timeline;
    timeline.duration_ms = 4000;
    oscilline::CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 1000;
    timeline.events.push_back(event);
    return timeline;
}

} // namespace

TEST_CASE("a missing settings file is the defaults") {
    const auto missing = temp_settings("absent.txt");
    const oscilline::SettingsLoad loaded = oscilline::load_settings(missing);
    CHECK(loaded.missing);
    CHECK(loaded.error.empty());
    CHECK_FALSE(loaded.settings.ribbon_guides);
    CHECK(loaded.settings.disc_camera);
    CHECK_FALSE(loaded.settings.fullscreen);
    CHECK(loaded.settings.aspect_ratio == oscilline::ViewportAspect::FourThree);
    CHECK(loaded.settings.window_scale == 0);
    CHECK(loaded.settings.playback.music_volume == 100);
    CHECK(loaded.settings.playback.sfx_volume == 100);
    CHECK(loaded.settings.playback.timing_offset_ms == 0);
    CHECK(loaded.settings.text_shake == oscilline::kTextShakeDefault);
    CHECK(loaded.settings.disc_assets);
    CHECK(loaded.settings.disc_path.empty());
    CHECK(loaded.settings.language_pak.empty());
    CHECK(loaded.settings.hud.score_coupons);
    CHECK_FALSE(loaded.settings.hud.timing_hints);
    CHECK_FALSE(loaded.settings.classic_resolution);
    CHECK_FALSE(loaded.settings.hud.control_hints);
    CHECK(loaded.settings.controls == oscilline::ControlBindings{});
    CHECK(oscilline::text_shake_scale(0) == doctest::Approx(0.f));
    // 1× is half the built-in amplitude. 2× is that amplitude.
    CHECK(oscilline::text_shake_scale(oscilline::kTextShakeDefault) == doctest::Approx(0.5f));
    CHECK(oscilline::text_shake_scale(oscilline::kTextShakeMax) == doctest::Approx(1.f));
    CHECK(oscilline::text_shake_scale(-4) == doctest::Approx(0.f));
    CHECK(oscilline::text_shake_scale(40) == doctest::Approx(1.f));
}

TEST_CASE("settings round-trip and unknown or invalid keys stay safe") {
    const auto file = temp_settings("round.txt");
    write_file(file,
               "# comment\n"
               "ribbon_guides=true\n"
               "not_a_real_key=1\n"
               "music_volume=nope\n"
               "sfx_volume=40\n"
               "music_volume=80\n"
               "timing_offset_ms=-12\n"
               "disc_camera=off\n"
               "fullscreen=yes\n"
               "aspect_ratio=16:9\n"
               "window_scale=2\n"
               "window_scale=99\n"
               "text_shake=4\n"
               "text_shake=30\n"
               "text_shake=nope\n"
               "disc_assets=false\n"
               "language=german\n"
               "disc_path=/games/my disc.cue\n"
               "this line is not a pair\n");
    const oscilline::SettingsLoad loaded = oscilline::load_settings(file);
    CHECK_FALSE(loaded.missing);
    CHECK(loaded.error.empty());
    CHECK(loaded.settings.ribbon_guides);
    CHECK(loaded.settings.playback.music_volume == 80);
    CHECK(loaded.settings.playback.sfx_volume == 40);
    CHECK(loaded.settings.playback.timing_offset_ms == -12);
    CHECK_FALSE(loaded.settings.disc_camera);
    CHECK(loaded.settings.fullscreen);
    CHECK(loaded.settings.aspect_ratio == oscilline::ViewportAspect::SixteenNine);
    // 99 is outside 0..8, so the earlier 2 remains.
    CHECK(loaded.settings.window_scale == 2);
    // 30 and "nope" are outside 0..20, so the earlier 4 remains.
    CHECK(loaded.settings.text_shake == 4);
    CHECK_FALSE(loaded.settings.disc_assets);
    const auto german = oscilline::language_pak_for("german");
    REQUIRE(german);
    CHECK(loaded.settings.language_pak == german.value());
    CHECK(loaded.settings.disc_path == "/games/my disc.cue");

    write_file(file, "!!!\nnot settings\n");
    const oscilline::SettingsLoad corrupt = oscilline::load_settings(file);
    CHECK_FALSE(corrupt.settings.ribbon_guides);
    CHECK(corrupt.settings.disc_camera);
    CHECK(corrupt.settings.playback.music_volume == 100);
    CHECK(corrupt.settings.text_shake == oscilline::kTextShakeDefault);

    oscilline::Settings saved = loaded.settings;
    saved.playback.timing_offset_ms = 7;
    saved.window_scale = 0;
    saved.text_shake = 0;
    saved.disc_path = "/games/my disc.cue";
    REQUIRE(oscilline::save_settings(file, saved));
    const oscilline::SettingsLoad again = oscilline::load_settings(file);
    CHECK(again.settings.ribbon_guides);
    CHECK(again.settings.playback.music_volume == 80);
    CHECK(again.settings.playback.sfx_volume == 40);
    CHECK(again.settings.playback.timing_offset_ms == 7);
    CHECK_FALSE(again.settings.disc_camera);
    CHECK(again.settings.fullscreen);
    CHECK(again.settings.aspect_ratio == oscilline::ViewportAspect::SixteenNine);
    CHECK(again.settings.window_scale == 0);
    CHECK(again.settings.text_shake == 0);
    CHECK_FALSE(again.settings.disc_assets);
    CHECK(again.settings.language_pak == german.value());
    CHECK(again.settings.disc_path == "/games/my disc.cue");
}

TEST_CASE("hud choices and control bindings round-trip") {
    using namespace oscilline;
    const auto file = temp_settings("controls.txt");
    write_file(file,
               "score_display=number\n"
               "score_display=bogus\n"
               "controller_hints=yes\n"
               "timing_hints=yes\n"
               "resolution=classic\n"
               "key_block=J\n"
               "key_wave=Space\n"
               "key_pit=Escape\n"
               "pad_loop=west\n"
               "pad_pit=start\n"
               "pad_wave=nonsense\n"
               "key_jump=K\n");
    const SettingsLoad loaded = load_settings(file);
    CHECK_FALSE(loaded.settings.hud.score_coupons);
    // controller_hints is the older key for control_hints.
    CHECK(loaded.settings.hud.control_hints);
    CHECK(loaded.settings.hud.timing_hints);
    CHECK(loaded.settings.classic_resolution);
    CHECK(loaded.settings.controls.keys[0] == "J");
    CHECK(loaded.settings.controls.keys[1] == "E");
    CHECK(loaded.settings.controls.keys[2] == "Space");
    // Escape and Start are reserved, and an unknown button is ignored.
    CHECK(loaded.settings.controls.keys[3] == "S");
    CHECK(loaded.settings.controls.pad[1] == PadButton::West);
    CHECK(loaded.settings.controls.pad[2] == PadButton::South);
    CHECK(loaded.settings.controls.pad[3] == PadButton::DpadDown);

    REQUIRE(save_settings(file, loaded.settings));
    const SettingsLoad again = load_settings(file);
    CHECK(again.settings.hud == loaded.settings.hud);
    CHECK(again.settings.classic_resolution);
    CHECK(again.settings.controls == loaded.settings.controls);

    // A file that swaps two keys loads as that swap, not as a shared key.
    write_file(file, "key_block=E\nkey_loop=Q\n");
    const SettingsLoad swapped = load_settings(file);
    CHECK(swapped.settings.controls.keys[0] == "E");
    CHECK(swapped.settings.controls.keys[1] == "Q");
}

TEST_CASE("command-line overrides win for the session and are not the saved copy") {
    oscilline::Settings stored;
    stored.ribbon_guides = false;
    stored.disc_camera = true;
    stored.disc_assets = true;
    stored.playback.music_volume = 70;

    oscilline::SettingsOverrides flags;
    flags.ribbon_guides = true;
    flags.disc_camera = false;
    flags.disc_assets = false;
    const oscilline::Settings effective = oscilline::apply_settings_overrides(stored, flags);
    CHECK(effective.ribbon_guides);
    CHECK_FALSE(effective.disc_camera);
    CHECK_FALSE(effective.disc_assets);
    CHECK(effective.playback.music_volume == 70);
    CHECK_FALSE(stored.ribbon_guides);
    CHECK(stored.disc_camera);
    CHECK(stored.disc_assets);

    oscilline::SettingsOverrides ribbon_only;
    ribbon_only.ribbon_guides = true;
    const oscilline::Settings camera_kept =
        oscilline::apply_settings_overrides(stored, ribbon_only);
    CHECK(camera_kept.ribbon_guides);
    CHECK(camera_kept.disc_camera);

    const auto file = temp_settings("override.txt");
    REQUIRE(oscilline::save_settings(file, stored));
    const oscilline::SettingsLoad loaded = oscilline::load_settings(file);
    CHECK_FALSE(loaded.settings.ribbon_guides);
    CHECK(loaded.settings.disc_camera);
    CHECK(loaded.settings.disc_assets);
}

TEST_CASE("calibration offset shifts a press and zero does not") {
    using namespace oscilline;
    CHECK(calibrated_press_ms(1000, 0) == 1000);
    CHECK(calibrated_press_ms(1000, 15) == 985);
    CHECK(calibrated_press_ms(1000, -20) == 1020);
    CHECK(nearest_click_ms(520, kCalibrationIntervalMs) == 500);
    CHECK(average_timing_offset(std::vector<int>{10, 20, 30}) == 20);
    CHECK(average_timing_offset({}) == 0);

    const CourseTimeline block = centered_block();
    const ObstacleWindow window = obstacle_window(block.events[0]);
    CHECK(window.perfect_open == 1000 - kPerfectWindowMs * 2 + kJudgmentLateBiasMs);
    CHECK(window.perfect_close == 1000 + kJudgmentLateBiasMs);
    CHECK(window.good_close == window.perfect_close + kGoodIntoObstacleMs);

    PlayState plain;
    play_advance(plain, block, 0, 1000, kActionBlock);
    PlayState zero;
    play_advance(zero, block, 0, 1000, kActionBlock, nullptr, 0);
    CHECK(zero.last == Judgment::Perfect);
    CHECK(zero.last == plain.last);
    CHECK(zero.score == plain.score);
    CHECK(zero.perfects == plain.perfects);
    CHECK(zero.event_index == plain.event_index);

    PlayState late_good;
    play_advance(late_good, block, 0, window.perfect_close + 10, kActionBlock, nullptr, 0);
    CHECK(late_good.last == Judgment::Good);

    PlayState pulled;
    play_advance(pulled, block, 0, 1100, kActionBlock, nullptr, 100);
    CHECK(pulled.last == Judgment::Perfect);
    CHECK(pulled.score == plain.score);

    PlayState missed;
    play_advance(missed, block, 0, 1200, kActionBlock, nullptr, 0);
    CHECK(missed.last == Judgment::Miss);

    // 1050 is past the good tail. Subtracting 20 ms pulls the press back into it.
    PlayState saved;
    play_advance(saved, block, 0, 1050, kActionBlock, nullptr, 20);
    CHECK(saved.last == Judgment::Good);
}