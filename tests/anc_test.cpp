// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// ANC parse, disc-camera projection, and the section schedule.

#include "course_draw.hpp"
#include "oscilline/anc.hpp"
#include "oscilline/asset/character.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/figure.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/inspect.hpp"
#include "oscilline/pak.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_i16(std::vector<std::uint8_t>& out, std::int16_t value) {
    put_u16(out, static_cast<std::uint16_t>(value));
}

std::vector<std::uint8_t> anc_bytes(std::uint16_t reserved,
                                    const std::vector<oscilline::AncKeyframe>& keys) {
    std::vector<std::uint8_t> bytes;
    put_u16(bytes, oscilline::kAncMagic);
    put_u16(bytes, reserved);
    put_u16(bytes, static_cast<std::uint16_t>(keys.size()));
    for (const oscilline::AncKeyframe& key : keys) {
        put_i16(bytes, key.eye_x);
        put_i16(bytes, key.eye_y);
        put_i16(bytes, key.eye_z);
        put_i16(bytes, key.target_x);
        put_i16(bytes, key.target_y);
        put_i16(bytes, key.target_z);
        put_i16(bytes, key.roll);
        put_i16(bytes, key.fov);
    }
    return bytes;
}

// The camera tests below are written looking along +Z with the road on +X.
// On the disc the road is world +Z, seen level from a look along -X (S01).
// This turns each key about Y, (x, y, z) -> (-z, y, x). The origin stays put,
// so every screen value is unchanged.
std::vector<std::uint8_t> road_bytes(std::uint16_t reserved,
                                     std::vector<oscilline::AncKeyframe> keys) {
    const auto turn = [](std::int16_t& x, std::int16_t& z) {
        const std::int16_t old_x = x;
        x = static_cast<std::int16_t>(-z);
        z = old_x;
    };
    for (oscilline::AncKeyframe& key : keys) {
        turn(key.eye_x, key.eye_z);
        turn(key.target_x, key.target_z);
    }
    return anc_bytes(reserved, keys);
}

oscilline::CameraPose disc_pose(std::int64_t time,
                                std::int32_t end_ms,
                                const oscilline::AncFile* intro,
                                const oscilline::AncFile* play,
                                bool use_disc,
                                bool track_clock = true,
                                float width = 640.f,
                                float height = 480.f) {
    oscilline::DiscCameraPaths paths;
    paths.intro = intro;
    paths.play = play;
    return oscilline::gameplay_camera(time, end_ms, width, height, track_clock, paths, use_disc);
}

// Projection distance of level_key's fov, in PS pixels.
float level_h() {
    return oscilline::anc_projection_h(256.f);
}

// Screen x (640 wide) of a point `units` right of the look axis, 800 ahead.
float right_of_center(float units) {
    return 320.f + units * level_h() / 800.f * 640.f / oscilline::kDiscBufferWidth;
}

oscilline::AncKeyframe level_key(std::int16_t roll = 0) {
    oscilline::AncKeyframe key;
    key.eye_x = 0;
    key.eye_y = 0;
    key.eye_z = -800;
    key.target_x = 0;
    key.target_y = 0;
    key.target_z = 0;
    key.roll = roll;
    key.fov = 256;
    return key;
}

// Constant-pitch orbit around the origin. Key 0 looks along +Z. The last key
// has come full circle, so the road angle unwinds to -360°.
oscilline::AncKeyframe orbit_key(int index, int count, std::int16_t roll = 0) {
    constexpr float kRadius = 800.f;
    constexpr float kPitch = 35.f * 3.14159265f / 180.f;
    const float theta =
        -2.f * 3.14159265f * static_cast<float>(index) / static_cast<float>(std::max(count - 1, 1));
    const float horiz = kRadius * std::cos(kPitch);
    const auto coord = [](float value) { return static_cast<std::int16_t>(std::lround(value)); };
    oscilline::AncKeyframe key = level_key(roll);
    key.eye_x = coord(horiz * std::sin(theta));
    key.eye_y = coord(-kRadius * std::sin(kPitch));
    key.eye_z = coord(-horiz * std::cos(theta));
    return key;
}

} // namespace

TEST_CASE("an ANC file is a 6-byte header plus 16-byte records") {
    oscilline::AncKeyframe key;
    key.eye_x = 10;
    key.eye_y = -20;
    key.eye_z = 30;
    key.target_x = 1;
    key.target_y = 2;
    key.target_z = 3;
    key.roll = 400;
    key.fov = 90;
    const std::vector<std::uint8_t> bytes = anc_bytes(7, {key});
    CHECK(bytes.size() == static_cast<std::size_t>(6 + 16));
    CHECK(bytes[0] == 0x00);
    CHECK(bytes[1] == 0x80);
    auto parsed = oscilline::parse_anc(bytes);
    REQUIRE(parsed);
    CHECK(parsed.value().magic == oscilline::kAncMagic);
    CHECK(parsed.value().key_count == 1);
    CHECK(parsed.value().reserved == 7);
    CHECK(parsed.value().keys.size() == 1);
    CHECK(parsed.value().keys[0].eye_x == 10);
    CHECK(parsed.value().keys[0].eye_y == -20);
    CHECK(parsed.value().keys[0].roll == 400);
    CHECK(parsed.value().keys[0].fov == 90);

    auto truncated = bytes;
    truncated.pop_back();
    auto short_file = oscilline::parse_anc(truncated);
    CHECK_FALSE(short_file);
    CHECK(short_file.error().find("15") != std::string::npos);
    auto extra = bytes;
    extra.push_back(0);
    auto long_file = oscilline::parse_anc(extra);
    CHECK_FALSE(long_file);
    CHECK(long_file.error().find("17") != std::string::npos);
    auto uneven = anc_bytes(0, {key, key});
    uneven.push_back(0);
    auto uneven_file = oscilline::parse_anc(uneven);
    CHECK_FALSE(uneven_file);
    CHECK(uneven_file.error().find("6-byte") != std::string::npos);
    CHECK_FALSE(oscilline::parse_anc({}));

    // Four bytes that begin with the magic used to be read as a count of 32768.
    const std::vector<std::uint8_t> magic_prefix = {0x00, 0x80, 0x00, 0x00};
    auto prefix = oscilline::parse_anc(magic_prefix);
    CHECK_FALSE(prefix);
    CHECK(prefix.error().find("truncated") != std::string::npos);
    CHECK(prefix.error().find("too large") == std::string::npos);

    const std::vector<std::uint8_t> zero_count = {0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
    auto zero = oscilline::parse_anc(zero_count);
    CHECK_FALSE(zero);
    CHECK(zero.error().find("zero") != std::string::npos);

    std::vector<std::uint8_t> bad_magic = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00};
    bad_magic.resize(6 + 16, 0);
    auto magic = oscilline::parse_anc(bad_magic);
    CHECK_FALSE(magic);
    CHECK(magic.error().find("0x8000") != std::string::npos);

    std::vector<std::uint8_t> wide = {0x00, 0x80, 0x00, 0x00, 0x01, 0x00};
    wide.resize(6 + 20, 0);
    auto wrong = oscilline::parse_anc(wide);
    CHECK_FALSE(wrong);
    CHECK(wrong.error().find("20") != std::string::npos);

    const std::vector<std::uint8_t> huge = {0x00, 0x80, 0x00, 0x00, 0x01, 0x10};
    auto too_many = oscilline::parse_anc(huge);
    CHECK_FALSE(too_many);
    CHECK(too_many.error().find("too large") != std::string::npos);
}

TEST_CASE("ANC samples lerp and take the short roll") {
    oscilline::AncKeyframe a;
    a.eye_x = 0;
    a.eye_y = 0;
    a.target_x = 0;
    a.roll = 4000;
    a.fov = 0;
    oscilline::AncKeyframe b = a;
    b.eye_x = 100;
    b.eye_y = 40;
    b.target_x = -20;
    b.roll = 100;
    b.fov = 50;
    auto parsed = oscilline::parse_anc(anc_bytes(0, {a, b}));
    REQUIRE(parsed);
    const oscilline::AncSample start = oscilline::sample_anc(parsed.value(), 0.f, false);
    CHECK(start.eye_x == doctest::Approx(0.f));
    CHECK(start.roll == doctest::Approx(4000.f));
    const oscilline::AncSample mid = oscilline::sample_anc(parsed.value(), 0.5f, false);
    CHECK(mid.eye_x == doctest::Approx(50.f));
    CHECK(mid.eye_y == doctest::Approx(20.f));
    CHECK(mid.target_x == doctest::Approx(-10.f));
    CHECK(mid.fov == doctest::Approx(25.f));
    CHECK(mid.roll == doctest::Approx(4098.f));
    const oscilline::AncSample end = oscilline::sample_anc(parsed.value(), 1.f, false);
    CHECK(end.eye_x == doctest::Approx(100.f));
    CHECK(end.roll == doctest::Approx(100.f));
    const oscilline::AncSample clamped = oscilline::sample_anc(parsed.value(), 4.f, false);
    CHECK(clamped.eye_x == doctest::Approx(100.f));
    const oscilline::AncSample lone =
        oscilline::sample_anc(oscilline::parse_anc(anc_bytes(0, {a})).value(), 0.5f, false);
    CHECK(lone.eye_x == doctest::Approx(0.f));
    CHECK(lone.roll == doctest::Approx(4000.f));
}

TEST_CASE("inspect prints an ANC camera") {
    oscilline::AncKeyframe key;
    key.eye_x = 12;
    key.roll = -4;
    oscilline::InspectRequest request;
    request.type = "anc";
    request.bytes = anc_bytes(3, {key});
    auto json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\"type\": \"anc\"") != std::string::npos);
    CHECK(json.value().find("\"magic\": 32768") != std::string::npos);
    CHECK(json.value().find("\"key_count\": 1") != std::string::npos);
    CHECK(json.value().find("\"record_bytes\": 16") != std::string::npos);
    CHECK(json.value().find("12") != std::string::npos);
}

TEST_CASE("the disc intro clock is 30 keys per second") {
    CHECK(oscilline::disc_camera_key_index(0) == doctest::Approx(0.f));
    CHECK(oscilline::disc_camera_key_index(-20) == doctest::Approx(0.f));
    CHECK(oscilline::disc_camera_key_index(500) == doctest::Approx(15.f));
    CHECK(oscilline::disc_camera_key_index(1000) == doctest::Approx(30.f));
    CHECK(oscilline::disc_camera_key_index(8000) == doctest::Approx(240.f));
    CHECK(oscilline::kDiscCameraKeysPerSecond == 30);

    // 240 keys cover 8.0 s. The index reaches the key count at the hand-off.
    CHECK(oscilline::disc_camera_in_intro(0, 240));
    CHECK(oscilline::disc_camera_in_intro(7999, 240));
    CHECK_FALSE(oscilline::disc_camera_in_intro(8000, 240));
    CHECK(oscilline::disc_camera_elapsed_ms(-oscilline::kCourseStartDelayMs, true) == 0);
    CHECK(oscilline::disc_camera_elapsed_ms(0, true) == oscilline::kCourseStartDelayMs);
    CHECK(oscilline::disc_camera_elapsed_ms(500, false) == 500);
    CHECK(oscilline::disc_camera_elapsed_ms(-5, false) == 0);

    std::vector<oscilline::AncKeyframe> keys(61, level_key());
    for (int i = 0; i < 61; ++i) {
        // Keep the look along +Z so the roll channel is the only tilt.
        keys[static_cast<std::size_t>(i)].eye_y = static_cast<std::int16_t>(i);
        keys[static_cast<std::size_t>(i)].target_y = static_cast<std::int16_t>(i);
    }
    keys[15].roll = 1024;
    auto intro = oscilline::parse_anc(road_bytes(30, keys));
    REQUIRE(intro);
    const oscilline::AncSample at_15 =
        oscilline::sample_anc_at(intro.value(), oscilline::disc_camera_key_index(500));
    CHECK(at_15.eye_y == doctest::Approx(15.f));
    CHECK(at_15.roll == doctest::Approx(1024.f));
    CHECK(oscilline::disc_camera_key_index(33) == doctest::Approx(0.99f));
    const oscilline::AncSample between =
        oscilline::sample_anc_at(intro.value(), oscilline::disc_camera_key_index(33));
    CHECK(between.eye_y == doctest::Approx(0.99f));

    auto play = oscilline::parse_anc(road_bytes(0, {level_key()}));
    REQUIRE(play);
    constexpr std::int32_t kEnd = 20000;
    const std::int64_t at_key_15 = 500 - oscilline::kCourseStartDelayMs;
    const oscilline::CameraPose sampled =
        disc_pose(at_key_15, kEnd, &intro.value(), &play.value(), true);
    CHECK(sampled.phase == oscilline::CameraPhase::Intro);
    CHECK(sampled.disc);
    CHECK(sampled.tilt_deg == doctest::Approx(-90.f));
    const oscilline::CameraPose other_song =
        disc_pose(at_key_15, 999999, &intro.value(), &play.value(), true);
    CHECK(other_song.tilt_deg == doctest::Approx(sampled.tilt_deg));
    CHECK(other_song.figure_x == doctest::Approx(sampled.figure_x));
    CHECK(other_song.phase == sampled.phase);
}

TEST_CASE("the built-in camera golden is unchanged without the disc flag") {
    constexpr float kWidth = 640.f;
    constexpr std::int32_t kEnd = 20000;
    const std::int64_t times[] = {-8000, -4000, -1, 0, 1000, kEnd - 1, kEnd, kEnd + 3000};
    auto intro = oscilline::parse_anc(anc_bytes(30, {level_key(1024)}));
    auto play = oscilline::parse_anc(anc_bytes(0, {level_key()}));
    REQUIRE(intro);
    REQUIRE(play);
    for (const std::int64_t time : times) {
        const oscilline::CameraPose plain = oscilline::camera_at(time, kEnd, kWidth, true);
        const oscilline::CameraPose held =
            disc_pose(time, kEnd, &intro.value(), &play.value(), false);
        const oscilline::CameraPose missing = disc_pose(time, kEnd, nullptr, nullptr, true);
        CHECK(held.phase == plain.phase);
        CHECK(held.tilt_deg == doctest::Approx(plain.tilt_deg));
        CHECK(held.figure_x == doctest::Approx(plain.figure_x));
        CHECK_FALSE(held.disc);
        CHECK(missing.phase == plain.phase);
        CHECK(missing.figure_x == doctest::Approx(plain.figure_x));
        CHECK(missing.tilt_deg == doctest::Approx(plain.tilt_deg));
        CHECK_FALSE(missing.disc);
    }
    const oscilline::CameraPose start = oscilline::camera_at(0, kEnd, kWidth);
    CHECK(start.phase == oscilline::CameraPhase::Intro);
    CHECK(start.tilt_deg == doctest::Approx(oscilline::kCameraIntroTiltDeg));
    CHECK(start.figure_x == doctest::Approx(kWidth * oscilline::kCameraIntroCenterFraction));
    const oscilline::CameraPose play_pose =
        oscilline::camera_at(oscilline::kCameraIntroMs, kEnd, kWidth);
    CHECK(play_pose.phase == oscilline::CameraPhase::Play);
    CHECK(play_pose.tilt_deg == doctest::Approx(0.f));
    CHECK(play_pose.figure_x == doctest::Approx(kWidth * oscilline::kHitXFraction));
    const oscilline::CameraPose track = oscilline::camera_at(0, kEnd, kWidth, true);
    CHECK(track.phase == oscilline::CameraPhase::Play);
    CHECK(track.tilt_deg == doctest::Approx(0.f));
    CHECK(track.figure_x == doctest::Approx(kWidth * oscilline::kHitXFraction));
}

TEST_CASE("a title-style dolly shortens look distance and keeps field order") {
    // Eye z only, from a far shot to a near-play shot. x/y, roll, and fov stay
    // put. These are the synthetic magnitudes from the field-order notes.
    oscilline::AncKeyframe far_key;
    far_key.eye_x = 120;
    far_key.eye_y = -40;
    far_key.eye_z = -30000;
    far_key.target_x = 120;
    far_key.target_y = -40;
    far_key.target_z = 0;
    far_key.roll = 0;
    far_key.fov = 534;
    oscilline::AncKeyframe near_key = far_key;
    near_key.eye_z = -3000;
    auto parsed = oscilline::parse_anc(anc_bytes(60, {far_key, near_key}));
    REQUIRE(parsed);
    REQUIRE(parsed.value().keys.size() == 2);
    CHECK(parsed.value().reserved == 60);
    const oscilline::AncKeyframe& far_out = parsed.value().keys[0];
    const oscilline::AncKeyframe& near_out = parsed.value().keys[1];
    CHECK(far_out.eye_x == 120);
    CHECK(far_out.eye_y == -40);
    CHECK(far_out.eye_z == -30000);
    CHECK(far_out.target_x == 120);
    CHECK(far_out.target_y == -40);
    CHECK(far_out.target_z == 0);
    CHECK(far_out.roll == 0);
    CHECK(far_out.fov == 534);
    CHECK(near_out.eye_x == far_out.eye_x);
    CHECK(near_out.eye_y == far_out.eye_y);
    CHECK(near_out.eye_z == -3000);
    CHECK(near_out.target_x == far_out.target_x);
    CHECK(near_out.target_y == far_out.target_y);
    CHECK(near_out.target_z == far_out.target_z);
    CHECK(near_out.roll == 0);
    CHECK(near_out.fov == 534);

    const oscilline::AncLook far_look =
        oscilline::anc_look(oscilline::sample_anc(parsed.value(), 0.f));
    const oscilline::AncLook near_look =
        oscilline::anc_look(oscilline::sample_anc(parsed.value(), 1.f));
    CHECK(near_look.distance < far_look.distance);
    CHECK(far_look.distance == doctest::Approx(30000.f));
    CHECK(near_look.distance == doctest::Approx(3000.f));

    constexpr float kWidth = 640.f;
    constexpr std::int32_t kEnd = 10000;
    const std::int64_t times[] = {-oscilline::kCameraIntroMs, 0, kEnd};
    for (const std::int64_t time : times) {
        const oscilline::CameraPose plain = oscilline::camera_at(time, kEnd, kWidth, true);
        const oscilline::CameraPose held = disc_pose(time, kEnd, &parsed.value(), nullptr, false);
        CHECK(held.tilt_deg == doctest::Approx(plain.tilt_deg));
        CHECK(held.figure_x == doctest::Approx(plain.figure_x));
        CHECK(held.phase == plain.phase);
    }
}

oscilline::DiscCameraFrame frame_of(const oscilline::AncKeyframe& key) {
    auto parsed = oscilline::parse_anc(road_bytes(0, {key}));
    REQUIRE(parsed);
    return oscilline::frame_disc_camera(
        oscilline::sample_anc_at(parsed.value(), 0.f), 640.f, 480.f);
}

TEST_CASE("disc projection keeps x unmirrored and rolls from the roll channel") {
    const oscilline::AncKeyframe level = level_key();
    const oscilline::DiscCameraFrame centered = frame_of(level);
    REQUIRE(centered.ok);
    CHECK(centered.figure_x ==
          doctest::Approx(oscilline::kDiscCenterX * 640.f / oscilline::kDiscBufferWidth));
    CHECK(centered.figure_y ==
          doctest::Approx(oscilline::kDiscCenterY * 480.f / oscilline::kDiscBufferHeight));
    CHECK(centered.tilt_deg == doctest::Approx(0.f));
    CHECK(centered.figure_x == doctest::Approx(320.f));
    CHECK(centered.figure_y == doctest::Approx(124.f * 480.f / 286.f));
    CHECK(centered.pixels_per_unit == doctest::Approx(level_h() / 800.f));

    // Origin sits on the +X side of a look axis at x = -100. Camera-right is
    // down x forward, so it lands right of center, as on the GTE.
    oscilline::AncKeyframe shifted = level;
    shifted.eye_x = -100;
    shifted.target_x = -100;
    const oscilline::DiscCameraFrame right = frame_of(shifted);
    REQUIRE(right.ok);
    CHECK(right.figure_x == doctest::Approx(right_of_center(100.f)));
    CHECK(right.figure_x > centered.figure_x);
    CHECK(right.tilt_deg == doctest::Approx(0.f));

    oscilline::AncKeyframe rolled = level;
    rolled.roll = 1024;
    const oscilline::DiscCameraFrame quarter = frame_of(rolled);
    REQUIRE(quarter.ok);
    CHECK(quarter.tilt_deg == doctest::Approx(-90.f));
    CHECK(quarter.figure_x == doctest::Approx(centered.figure_x));
    CHECK(quarter.figure_y == doctest::Approx(centered.figure_y));

    oscilline::AncKeyframe half_roll = level;
    half_roll.roll = 512;
    CHECK(frame_of(half_roll).tilt_deg == doctest::Approx(-45.f));

    // A pitched look straight down the play view leaves the road horizontal. The roll channel spins
    // it.
    oscilline::AncKeyframe pitched = level;
    pitched.eye_y = -300;
    pitched.target_y = -300;
    const oscilline::DiscCameraFrame raised = frame_of(pitched);
    REQUIRE(raised.ok);
    CHECK(raised.tilt_deg == doctest::Approx(0.f));
    CHECK(raised.figure_x == doctest::Approx(centered.figure_x));
    CHECK(raised.figure_y == doctest::Approx((124.f + 300.f * level_h() / 800.f) * 480.f / 286.f));

    pitched.roll = 1024;
    const oscilline::DiscCameraFrame raised_roll = frame_of(pitched);
    REQUIRE(raised_roll.ok);
    CHECK(raised_roll.tilt_deg == doctest::Approx(-90.f));
    CHECK(raised_roll.figure_x == doctest::Approx(right_of_center(300.f)));
    CHECK(raised_roll.figure_y == doctest::Approx(124.f * 480.f / 286.f));
}

TEST_CASE("the fov channel is an inverse projection distance") {
    CHECK(oscilline::anc_projection_h(500.f) == doctest::Approx(500.f));
    CHECK(oscilline::anc_projection_h(1000.f) == doctest::Approx(250.f));
    CHECK(oscilline::anc_projection_h(0.f) == doctest::Approx(0.f));
    // A wider fov value draws the same scene smaller, as the intro spin does on the disc.
    oscilline::AncKeyframe narrow = level_key();
    narrow.fov = 500;
    oscilline::AncKeyframe wide = level_key();
    wide.fov = 1000;
    const oscilline::DiscCameraFrame near_frame = frame_of(narrow);
    const oscilline::DiscCameraFrame far_frame = frame_of(wide);
    REQUIRE(near_frame.ok);
    REQUIRE(far_frame.ok);
    CHECK(far_frame.pixels_per_unit == doctest::Approx(near_frame.pixels_per_unit * 0.5f));
}

TEST_CASE("the intro hands off to a static play pose with no song time") {
    constexpr std::int32_t kEnd = 12000;
    std::vector<oscilline::AncKeyframe> intro_keys(30, level_key());
    for (oscilline::AncKeyframe& key : intro_keys) {
        key.eye_z = -400;
    }
    auto intro = oscilline::parse_anc(road_bytes(30, intro_keys));
    REQUIRE(intro);

    oscilline::AncKeyframe play_key = level_key();
    play_key.eye_x = -100;
    play_key.target_x = -100;
    oscilline::AncKeyframe later = play_key;
    later.eye_x = 400;
    later.target_x = 400;
    later.roll = 1024;
    oscilline::AncKeyframe later_still = later;
    later_still.eye_z = -200;
    auto play = oscilline::parse_anc(road_bytes(0, {play_key, later, later_still}));
    REQUIRE(play);

    const std::int64_t start = -oscilline::kCourseStartDelayMs;
    const oscilline::CameraPose at_start =
        disc_pose(start, kEnd, &intro.value(), &play.value(), true);
    CHECK(at_start.phase == oscilline::CameraPhase::Intro);
    CHECK(at_start.disc);
    CHECK(at_start.tilt_deg == doctest::Approx(0.f));
    CHECK(at_start.figure_x == doctest::Approx(320.f));
    CHECK(at_start.zoom == doctest::Approx(2.f));

    const oscilline::CameraPose late_intro =
        disc_pose(start + 999, kEnd, &intro.value(), &play.value(), true);
    CHECK(late_intro.phase == oscilline::CameraPhase::Intro);
    CHECK(late_intro.figure_x == doctest::Approx(at_start.figure_x));
    CHECK(late_intro.zoom == doctest::Approx(at_start.zoom));

    const oscilline::CameraPose handed =
        disc_pose(start + 1000, kEnd, &intro.value(), &play.value(), true);
    CHECK(handed.phase == oscilline::CameraPhase::Play);
    CHECK(handed.figure_x == doctest::Approx(right_of_center(100.f)));
    CHECK(handed.tilt_deg == doctest::Approx(0.f));
    CHECK(handed.zoom == doctest::Approx(1.f));
    CHECK(handed.figure_x != doctest::Approx(at_start.figure_x));

    const oscilline::CameraPose later_play =
        disc_pose(start + 5000, 1000, &intro.value(), &play.value(), true);
    const oscilline::CameraPose other_end =
        disc_pose(start + 5000, 80000, &intro.value(), &play.value(), true);
    const oscilline::CameraPose much_later =
        disc_pose(start + 50000, kEnd, &intro.value(), &play.value(), true);
    CHECK(later_play.phase == oscilline::CameraPhase::Play);
    CHECK(later_play.figure_x == doctest::Approx(handed.figure_x));
    CHECK(later_play.figure_y == doctest::Approx(handed.figure_y));
    CHECK(later_play.tilt_deg == doctest::Approx(handed.tilt_deg));
    CHECK(later_play.zoom == doctest::Approx(handed.zoom));
    CHECK(other_end.figure_x == doctest::Approx(handed.figure_x));
    CHECK(other_end.tilt_deg == doctest::Approx(handed.tilt_deg));
    CHECK(other_end.phase == oscilline::CameraPhase::Play);
    CHECK(much_later.figure_x == doctest::Approx(handed.figure_x));
    CHECK(much_later.tilt_deg == doctest::Approx(0.f));

    // Past the built-in outro the disc pose stays the play camera.
    const oscilline::CameraPose after =
        disc_pose(static_cast<std::int64_t>(kEnd) + oscilline::kCameraOutroMs,
                  kEnd,
                  &intro.value(),
                  &play.value(),
                  true);
    CHECK(after.phase == oscilline::CameraPhase::Play);
    CHECK(after.figure_x == doctest::Approx(handed.figure_x));
    CHECK(after.tilt_deg == doctest::Approx(0.f));
    const oscilline::CameraPose builtin_outro = oscilline::camera_at(
        static_cast<std::int64_t>(kEnd) + oscilline::kCameraOutroMs, kEnd, 640.f, true);
    CHECK(builtin_outro.phase == oscilline::CameraPhase::Outro);
    CHECK(builtin_outro.tilt_deg != doctest::Approx(after.tilt_deg));

    // Without the track clock, elapsed is now_ms itself. Song length still does not matter.
    const oscilline::CameraPose direct =
        disc_pose(0, kEnd, &intro.value(), &play.value(), true, false);
    CHECK(direct.phase == oscilline::CameraPhase::Intro);
    CHECK(direct.zoom == doctest::Approx(2.f));
    const oscilline::CameraPose direct_play =
        disc_pose(1000, 50, &intro.value(), &play.value(), true, false);
    CHECK(direct_play.phase == oscilline::CameraPhase::Play);
    CHECK(direct_play.figure_x == doctest::Approx(handed.figure_x));
}

TEST_CASE("the intro road angle unwinds a full turn and the figure stays on screen") {
    constexpr int kKeys = 240;
    constexpr std::int32_t kEnd = 20000;
    constexpr float kWidth = 640.f;
    constexpr float kHeight = 480.f;
    std::vector<oscilline::AncKeyframe> intro_keys;
    intro_keys.reserve(static_cast<std::size_t>(kKeys));
    std::vector<oscilline::AncKeyframe> rolled_keys;
    rolled_keys.reserve(static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        intro_keys.push_back(orbit_key(i, kKeys));
        rolled_keys.push_back(orbit_key(i, kKeys, 1024));
    }
    auto intro = oscilline::parse_anc(road_bytes(30, intro_keys));
    auto rolled = oscilline::parse_anc(road_bytes(30, rolled_keys));
    oscilline::AncKeyframe play_key = level_key();
    play_key.eye_x = -100;
    play_key.target_x = -100;
    auto play = oscilline::parse_anc(road_bytes(0, {play_key}));
    REQUIRE(intro);
    REQUIRE(rolled);
    REQUIRE(play);

    const std::int64_t start = -oscilline::kCourseStartDelayMs;
    const auto at = [&](std::int64_t elapsed_ms) {
        return disc_pose(start + elapsed_ms, kEnd, &intro.value(), &play.value(), true);
    };
    const oscilline::CameraPose t0 = at(0);
    const oscilline::CameraPose t2 = at(2000);
    const oscilline::CameraPose t4 = at(4000);
    const oscilline::CameraPose t6 = at(6000);
    const oscilline::CameraPose t8 = at(8000);
    const oscilline::CameraPose later = at(12000);
    const float center_y = oscilline::kDiscCenterY * kHeight / oscilline::kDiscBufferHeight;
    CHECK(t0.phase == oscilline::CameraPhase::Intro);
    CHECK(t0.tilt_deg == doctest::Approx(0.f).epsilon(0.02f));
    CHECK(t0.figure_x == doctest::Approx(320.f));
    CHECK(t0.figure_y == doctest::Approx(center_y));
    CHECK(t2.figure_x == doctest::Approx(t0.figure_x));
    CHECK(t2.figure_y == doctest::Approx(t0.figure_y));
    CHECK(t4.figure_x == doctest::Approx(t0.figure_x));
    CHECK(t4.figure_y == doctest::Approx(t0.figure_y));
    CHECK(t6.figure_x == doctest::Approx(t0.figure_x));
    CHECK(t6.figure_y == doctest::Approx(t0.figure_y));
    CHECK(t8.figure_y == doctest::Approx(center_y));
    CHECK(t2.tilt_deg < t0.tilt_deg);
    CHECK(t4.tilt_deg < t2.tilt_deg);
    CHECK(t6.tilt_deg < t4.tilt_deg);
    CHECK(t2.tilt_deg == doctest::Approx(-90.6f).epsilon(0.02f));
    CHECK(t4.tilt_deg == doctest::Approx(-180.5f).epsilon(0.02f));
    CHECK(t6.tilt_deg == doctest::Approx(-272.f).epsilon(0.02f));

    float previous = 1.f;
    for (int ms = 0; ms < 8000; ms += 200) {
        const oscilline::CameraPose pose = at(ms);
        CHECK(pose.phase == oscilline::CameraPhase::Intro);
        CHECK(pose.tilt_deg <= previous + 0.05f);
        CHECK(pose.figure_y > 0.f);
        CHECK(pose.figure_y < kHeight);
        CHECK(pose.figure_x > 0.f);
        CHECK(pose.figure_x < kWidth);
        previous = pose.tilt_deg;
    }
    const oscilline::CameraPose just_before = at(7999);
    CHECK(just_before.phase == oscilline::CameraPhase::Intro);
    CHECK(just_before.tilt_deg == doctest::Approx(-360.f).epsilon(0.01f));
    CHECK(just_before.tilt_deg < previous + 0.05f);

    // 8 s is the hand-off. Play is the static S01 pose, level, not -360.
    CHECK(t8.phase == oscilline::CameraPhase::Play);
    CHECK(t8.tilt_deg == doctest::Approx(0.f));
    CHECK(t8.figure_x == doctest::Approx(right_of_center(100.f)));
    CHECK(t8.zoom == doctest::Approx(1.f));
    CHECK(later.figure_x == doctest::Approx(t8.figure_x));
    CHECK(later.figure_y == doctest::Approx(t8.figure_y));
    CHECK(later.tilt_deg == doctest::Approx(t8.tilt_deg));
    const oscilline::CameraPose other_song =
        disc_pose(start + 4000, 99999, &intro.value(), &play.value(), true);
    CHECK(other_song.tilt_deg == doctest::Approx(t4.tilt_deg));
    CHECK(other_song.figure_y == doctest::Approx(t4.figure_y));

    // A constant quarter-turn of roll rides on the same orbit.
    const oscilline::CameraPose rolled_start =
        disc_pose(start, kEnd, &rolled.value(), &play.value(), true);
    const oscilline::CameraPose rolled_end =
        disc_pose(start + 7999, kEnd, &rolled.value(), &play.value(), true);
    CHECK(rolled_start.tilt_deg == doctest::Approx(-90.f).epsilon(0.02f));
    CHECK(rolled_end.tilt_deg == doctest::Approx(-450.f).epsilon(0.02f));
    CHECK(rolled_end.tilt_deg < rolled_start.tilt_deg);

    const oscilline::CameraPose builtin = oscilline::camera_at(start + 4000, kEnd, kWidth, true);
    const oscilline::CameraPose flag_off =
        disc_pose(start + 4000, kEnd, &intro.value(), &play.value(), false);
    CHECK(flag_off.tilt_deg == doctest::Approx(builtin.tilt_deg));
    CHECK(flag_off.figure_x == doctest::Approx(builtin.figure_x));
    CHECK(flag_off.phase == builtin.phase);
    CHECK_FALSE(flag_off.disc);
}

TEST_CASE("a turned intro keeps the figure near mid-height and clamps a grazing zoom") {
    constexpr float kWidth = 640.f;
    constexpr float kHeight = 480.f;
    // Look of a pitched orbit at 120° of yaw, with the eye beside the origin so
    // the origin is off screen and very close. The character is the aim point.
    oscilline::AncKeyframe grazing = level_key();
    grazing.eye_x = -118;
    grazing.eye_y = -34;
    grazing.eye_z = -105;
    grazing.target_x = 1656;
    grazing.target_y = 1400;
    grazing.target_z = -1129;
    std::vector<oscilline::AncKeyframe> keys(30, grazing);
    auto intro = oscilline::parse_anc(road_bytes(30, keys));
    auto play = oscilline::parse_anc(road_bytes(0, {level_key()}));
    REQUIRE(intro);
    REQUIRE(play);
    const oscilline::CameraPose pose =
        disc_pose(0, 20000, &intro.value(), &play.value(), true, false, kWidth, kHeight);
    CHECK(pose.phase == oscilline::CameraPhase::Intro);
    CHECK(pose.disc);
    CHECK(pose.tilt_deg < -90.f);
    const float center_y = oscilline::kDiscCenterY * kHeight / oscilline::kDiscBufferHeight;
    CHECK(pose.figure_y == doctest::Approx(center_y).epsilon(0.02f));
    CHECK(pose.figure_x == doctest::Approx(320.f).epsilon(0.02f));
    CHECK(pose.figure_y > 0.f);
    CHECK(pose.figure_y < kHeight);
    CHECK(pose.zoom >= oscilline::kDiscIntroZoomMin);
    CHECK(pose.zoom <= oscilline::kDiscIntroZoomMax);

    // The framed dolly in the hand-off test is aimed at the origin and stays at 2x.
    std::vector<oscilline::AncKeyframe> dolly(30, level_key());
    for (oscilline::AncKeyframe& key : dolly) {
        key.eye_z = -400;
    }
    auto close = oscilline::parse_anc(road_bytes(30, dolly));
    REQUIRE(close);
    const oscilline::CameraPose pushed =
        disc_pose(0, 20000, &close.value(), &play.value(), true, false);
    CHECK(pushed.zoom == doctest::Approx(2.f));
    CHECK(pushed.tilt_deg == doctest::Approx(0.f));
}

TEST_CASE("the projected foot lies on the projected ribbon at every camera key") {
    constexpr int kKeys = 240;
    std::vector<oscilline::AncKeyframe> intro_keys;
    intro_keys.reserve(static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        // The disc's spin pivots on a point ahead of and above the figure, so
        // the world origin does not project to the screen center.
        oscilline::AncKeyframe key = orbit_key(i, kKeys);
        key.target_x = 150;
        key.target_y = -60;
        key.eye_x = static_cast<std::int16_t>(key.eye_x + 150);
        key.eye_y = static_cast<std::int16_t>(key.eye_y - 60);
        intro_keys.push_back(key);
    }
    auto intro = oscilline::parse_anc(road_bytes(30, intro_keys));
    auto play = oscilline::parse_anc(road_bytes(0, {level_key()}));
    REQUIRE(intro);
    REQUIRE(play);

    // A red foot along the road on the ground plane. Projection still plants it
    // on the ribbon; the drawn model is then lifted by the ribbon clearance.
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, -30}, {0, 0, 30}};
    oscilline::TmdPrimitive foot;
    foot.kind = oscilline::PrimitiveKind::Line;
    foot.vertex_indices = {0, 1};
    foot.color = 0x000000FFu;
    object.primitives.push_back(foot);
    model.objects.push_back(object);
    oscilline::AnmFile clip;
    clip.unk1 = 30;
    clip.frame_count = 1;
    clip.frames.resize(1);
    clip.frames[0].keys.push_back(oscilline::AnmKeyframe{});
    oscilline::DiscFigurePose figure;
    figure.model = &model;
    figure.clip = &clip;
    figure.loop = false;
    figure.placement.valid = true;
    figure.placement.scale = 1.f;

    oscilline::CourseTimeline course;
    course.duration_ms = 20000;
    oscilline::PlayState play_state;
    constexpr float kPsPx = static_cast<float>(oscilline::kLogicalHeight) / 286.f;
    const std::int64_t start = -oscilline::kCourseStartDelayMs;
    for (const std::int64_t time : {start + 500,
                                    start + 2000,
                                    start + 3000,
                                    start + 4000,
                                    start + 5000,
                                    start + 6500,
                                    start + 7900,
                                    std::int64_t{1000},
                                    std::int64_t{6000}}) {
        CAPTURE(time);
        oscilline::CourseView view;
        view.figure = &figure;
        view.cameras.intro = &intro.value();
        view.cameras.play = &play.value();
        view.disc_camera = true;
        const oscilline::CourseFrame frame = oscilline::draw_course(course, play_state, time, view);
        double wx = 0.0;
        double wy = 0.0;
        double rx = 0.0;
        double ry = 0.0;
        int whites = 0;
        int reds = 0;
        for (const oscilline::Vertex& v : frame.triangles.vertices) {
            if (v.r > 0.95f && v.g > 0.95f && v.b > 0.95f) {
                wx += v.x;
                wy += v.y;
                ++whites;
            } else if (v.r > 0.95f && v.g < 0.05f && v.b < 0.05f) {
                rx += v.x;
                ry += v.y;
                ++reds;
            }
        }
        REQUIRE(whites > 0);
        REQUIRE(reds > 0);
        wx /= whites;
        wy /= whites;
        rx /= reds;
        ry /= reds;
        // Principal axis of the ribbon vertices.
        double sxx = 0.0;
        double sxy = 0.0;
        double syy = 0.0;
        for (const oscilline::Vertex& v : frame.triangles.vertices) {
            if (v.r > 0.95f && v.g > 0.95f && v.b > 0.95f) {
                const double dx = v.x - wx;
                const double dy = v.y - wy;
                sxx += dx * dx;
                sxy += dx * dy;
                syy += dy * dy;
            }
        }
        const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
        const double off = -(rx - wx) * std::sin(angle) + (ry - wy) * std::cos(angle);
        // Screen-up clearance, measured along the ribbon normal. One PS pixel of slack.
        const double lifted =
            off + static_cast<double>(oscilline::kFigureRibbonClearancePx) * std::cos(angle);
        CHECK(std::fabs(lifted) <= kPsPx);
    }
}

TEST_CASE("a road camera slot parses and a short file falls back") {
    oscilline::PakArchive archive;
    oscilline::PakEntry entry;
    entry.name = "ROAD/B01.ANC";
    oscilline::AncKeyframe key;
    key.eye_z = 8;
    entry.data = anc_bytes(1, {key});
    archive.entries.push_back(entry);
    oscilline::PakEntry intro_entry;
    intro_entry.name = "ROAD/TV_SS.ANC";
    intro_entry.data = anc_bytes(30, {level_key()});
    archive.entries.push_back(intro_entry);
    oscilline::PakEntry play_entry;
    play_entry.name = "ROAD/S01.ANC";
    play_entry.data = anc_bytes(0, {level_key()});
    archive.entries.push_back(play_entry);
    oscilline::AssetRegistry::Parts parts;
    parts.game = &archive;
    parts.game_pak_path = "GAME/02_FILES.PAK";
    auto registry = oscilline::AssetRegistry::from_parts(parts);
    CHECK(registry.origin(oscilline::Slot::CameraRoad) == oscilline::AssetOrigin::Disc);
    const oscilline::AncFile* camera = registry.camera(oscilline::Slot::CameraRoad);
    REQUIRE(camera != nullptr);
    CHECK(camera->keys.size() == 1);
    CHECK(camera->keys[0].eye_z == 8);
    CHECK(registry.origin(oscilline::Slot::CameraIntro) == oscilline::AssetOrigin::Disc);
    CHECK(registry.origin(oscilline::Slot::CameraPlay) == oscilline::AssetOrigin::Disc);
    REQUIRE(registry.camera(oscilline::Slot::CameraIntro) != nullptr);
    REQUIRE(registry.camera(oscilline::Slot::CameraPlay) != nullptr);
    CHECK(registry.camera(oscilline::Slot::CameraIntro)->keys.size() == 1);
    CHECK(registry.camera(oscilline::Slot::CameraPlay)->keys[0].fov == 256);

    archive.entries[0].data = {0x01, 0x02, 0x03, 0x04};
    auto broken = oscilline::AssetRegistry::from_parts(parts);
    CHECK(broken.origin(oscilline::Slot::CameraRoad) == oscilline::AssetOrigin::Disc);
    CHECK(broken.camera(oscilline::Slot::CameraRoad) == nullptr);
    CHECK(broken.origin(oscilline::Slot::CameraRoad) == oscilline::AssetOrigin::Placeholder);
    CHECK_FALSE(broken.warnings().empty());
}

namespace {

int count_looks(std::size_t key_count, int step) {
    int drawn = 0;
    for (std::size_t i = 0; i < key_count; ++i) {
        if (oscilline::anc_overlay_draws_look(i, key_count, step)) {
            ++drawn;
        }
    }
    return drawn;
}

} // namespace

TEST_CASE("ANC overlay stride thins a long path to about fifty looks") {
    CHECK(oscilline::anc_look_step(0) == 1);
    CHECK(oscilline::anc_look_step(1) == 1);
    CHECK(oscilline::anc_look_step(50) == 1);
    CHECK(oscilline::anc_look_step(100) == 2);

    // A road path is about 780 keys. The count is synthetic: no disc bytes.
    constexpr std::size_t kRoadKeys = 780;
    const int step = oscilline::anc_look_step(kRoadKeys);
    const int drawn = count_looks(kRoadKeys, step);
    CHECK(step == 16);
    CHECK(count_looks(kRoadKeys, 1) == static_cast<int>(kRoadKeys));
    CHECK(drawn == 50);
    CHECK(drawn >= 40);
    CHECK(drawn <= 60);
    CHECK(oscilline::anc_overlay_draws_look(0, kRoadKeys, step));
    CHECK(oscilline::anc_overlay_draws_look(kRoadKeys - 1, kRoadKeys, step));
    CHECK_FALSE(oscilline::anc_overlay_draws_look(1, kRoadKeys, step));
    // An explicit stride still keeps the last key when it misses the grid.
    CHECK(count_looks(kRoadKeys, 20) == 40);
    CHECK(oscilline::anc_overlay_draws_look(kRoadKeys - 1, kRoadKeys, 20));
}

TEST_CASE("a section schedule samples the stretched key and holds S01 when S02 is missing") {
    using namespace oscilline;
    std::vector<AncKeyframe> play_keys(static_cast<std::size_t>(kObservedS01Keys), level_key());
    for (int i = 0; i < kObservedS01Keys; ++i) {
        play_keys[static_cast<std::size_t>(i)].eye_y = static_cast<std::int16_t>(i);
    }
    std::vector<AncKeyframe> intro_keys(static_cast<std::size_t>(kObservedTransitionKeys),
                                        level_key());
    for (int i = 0; i < kObservedTransitionKeys; ++i) {
        intro_keys[static_cast<std::size_t>(i)].eye_x = static_cast<std::int16_t>(i);
    }
    auto play = parse_anc(anc_bytes(0, play_keys));
    auto intro = parse_anc(anc_bytes(30, intro_keys));
    REQUIRE(play);
    REQUIRE(intro);

    const std::vector<CameraSectionSpan> sections = {
        {8420, 48290},
        {62040, 101780},
        {120710, 160000},
    };
    DiscCameraPaths paths;
    paths.intro = &intro.value();
    paths.play = &play.value();
    paths.course_index = 0;
    paths.sections = sections;

    const DiscCameraSample start = disc_camera_sample(8420, true, paths);
    CHECK(start.valid);
    CHECK(start.key_index == doctest::Approx(0.f));
    CHECK(start.sample.eye_y == doctest::Approx(0.f));
    CHECK_FALSE(start.intro_phase);

    const DiscCameraSample end = disc_camera_sample(48290, true, paths);
    CHECK(end.sample.eye_y == doctest::Approx(25.f));

    const DiscCameraSample held = disc_camera_sample(50000, true, paths);
    CHECK(held.sample.eye_y == doctest::Approx(25.f));
    CHECK(held.key_index == doctest::Approx(25.f));

    const DiscCameraSample spinning = disc_camera_sample(55040, true, paths);
    CHECK(spinning.intro_phase);
    CHECK(spinning.sample.eye_x == doctest::Approx(30.f));

    const DiscCameraSample handed = disc_camera_sample(62040, true, paths);
    CHECK_FALSE(handed.intro_phase);
    CHECK(handed.key_index == doctest::Approx(0.f));
    CHECK(handed.sample.eye_y == doctest::Approx(0.f));

    // Course 2 section 2 wants S02. With that file absent the pose is S01 at key 0.
    const std::vector<CameraSectionSpan> course2 = {
        {8140, 47830},
        {69210, 109680},
        {118090, 160000},
    };
    paths.course_index = 1;
    paths.sections = course2;
    paths.s02 = nullptr;
    const std::int64_t mid = 69210 + (109680 - 69210) / 2;
    const DiscCameraSample fallback = disc_camera_sample(mid, true, paths);
    CHECK(fallback.valid);
    CHECK(fallback.key_index == doctest::Approx(0.f));
    CHECK(fallback.sample.eye_y == doctest::Approx(0.f));
    CHECK_FALSE(fallback.intro_phase);
}

TEST_CASE("the gold shift moves the disc figure by the video estimates") {
    using namespace oscilline;
    auto play = parse_anc(anc_bytes(0, {level_key()}));
    REQUIRE(play);
    DiscCameraPaths paths;
    paths.play = &play.value();
    const std::int64_t when = kGoldFigureShiftStartMs + kGoldFigureShiftMs;
    const CameraPose plain = gameplay_camera(when, 200000, 640.f, 480.f, true, paths, true);
    paths.gold = true;
    const CameraPose shifted = gameplay_camera(when, 200000, 640.f, 480.f, true, paths, true);
    CHECK(plain.disc);
    CHECK(shifted.figure_x ==
          doctest::Approx(plain.figure_x + kGoldFigureShiftPs * 640.f / kDiscBufferWidth));
    CHECK(shifted.figure_y ==
          doctest::Approx(plain.figure_y + kGoldRibbonRisePs * 480.f / kDiscBufferHeight));

    const CameraPose early = gameplay_camera(10000, 200000, 640.f, 480.f, true, paths, true);
    const CameraPose early_plain = gameplay_camera(10000, 200000, 640.f, 480.f, true, paths, false);
    paths.gold = false;
    const CameraPose early_off = gameplay_camera(10000, 200000, 640.f, 480.f, true, paths, true);
    CHECK(early.figure_x == doctest::Approx(early_off.figure_x));
    CHECK(early.figure_y == doctest::Approx(early_off.figure_y));
    CHECK(early_plain.disc == false);
}
