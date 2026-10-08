// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Meter curves and the confidence gate around them.

#include "oscilline/anm.hpp"
#include "oscilline/asset/meter.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/text/vector_font.hpp"
#include "oscilline/tmd.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <span>
#include <vector>

namespace {

struct Buf {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value));
        u16(static_cast<std::uint16_t>(value >> 16));
    }

    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
};

struct Stroke {
    std::int16_t x0 = 0;
    std::int16_t y0 = 0;
    std::int16_t x1 = 0;
    std::int16_t y1 = 0;
    std::uint32_t color = 0x00FFFFFFu;
    bool gouraud = false;
    std::uint32_t extra = 0;
};

Buf line_objects(std::span<const Stroke> strokes) {
    Buf file;
    const auto count = static_cast<std::uint32_t>(strokes.size());
    file.u32(0x41);
    file.u32(0);
    file.u32(count);
    const std::uint32_t table = count * 0x1Cu;
    std::uint32_t cursor = table;
    for (const Stroke& stroke : strokes) {
        const std::uint32_t packet = stroke.gouraud ? 16u : 12u;
        file.u32(cursor);
        file.u32(2);
        file.u32(0);
        file.u32(0);
        file.u32(cursor + 16u);
        file.u32(1);
        file.i32(0);
        cursor += 16u + packet;
    }
    for (const Stroke& stroke : strokes) {
        file.i16(stroke.x0);
        file.i16(stroke.y0);
        file.i16(0);
        file.u16(0);
        file.i16(stroke.x1);
        file.i16(stroke.y1);
        file.i16(0);
        file.u16(0);
        file.u8(stroke.gouraud ? 4 : 3);
        file.u8(stroke.gouraud ? 3 : 2);
        file.u8(1);
        file.u8(stroke.gouraud ? 0x50 : 0x40);
        file.u32(stroke.color);
        if (stroke.gouraud) {
            file.u32(stroke.extra);
        }
        file.u16(0);
        file.u16(1);
    }
    return file;
}

std::vector<std::uint8_t> keyed_anm(int frames) {
    Buf file;
    file.u16(0x8000);
    file.u16(30);
    file.u16(static_cast<std::uint16_t>(frames));
    const int header_units = 3 + frames + 1;
    for (int frame = 0; frame <= frames; ++frame) {
        file.u16(static_cast<std::uint16_t>(header_units + frame * 4));
    }
    for (int frame = 0; frame < frames; ++frame) {
        file.u8(0);
        file.u8(0x04);
        file.i16(static_cast<std::int16_t>(frame));
        file.i16(0);
        file.i16(0);
    }
    return file.bytes;
}

} // namespace

TEST_CASE("meter fill selects the first, middle, and last frame") {
    CHECK(oscilline::meter_frame_index(0.f, 5) == 0);
    CHECK(oscilline::meter_frame_index(0.5f, 5) == 2);
    CHECK(oscilline::meter_frame_index(1.f, 5) == 4);
    CHECK(oscilline::meter_frame_index(-1.f, 5) == 0);
    CHECK(oscilline::meter_frame_index(2.f, 4) == 3);
    CHECK(oscilline::meter_frame_index(0.5f, 1) == 0);
    CHECK(oscilline::meter_frame_index(0.5f, 0) == 0);
}

TEST_CASE("an unkeyed meter object stays hidden on the chosen frame") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 4, 6, 4}};
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    auto animation = oscilline::parse_anm(keyed_anm(5));
    REQUIRE(animation);
    const int frame = oscilline::meter_frame_index(1.f, 5);
    CHECK(frame == 4);
    const auto poses = oscilline::poses_for_frame(animation.value(), 2, frame, 0.f, false);
    REQUIRE(poses.size() == 2);
    CHECK(poses[0].visible);
    CHECK_FALSE(poses[1].visible);
    CHECK(oscilline::tmd_wireframe(parsed.value(), poses).size() == 1);
}

TEST_CASE("packet color is white unless the wireframe is asked to copy it") {
    const Stroke strokes[] = {{0, 0, 10, 0, 0x000000FFu, true, 0x0000FF00u}};
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    const auto plain = oscilline::tmd_wireframe(parsed.value());
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].color.r == doctest::Approx(1.f));
    CHECK(plain[0].color.g == doctest::Approx(1.f));
    CHECK(plain[0].color.b == doctest::Approx(1.f));

    oscilline::WireframeOptions options;
    options.packet_color = true;
    const auto colored = oscilline::tmd_wireframe(parsed.value(), {}, options);
    REQUIRE(colored.size() == 1);
    CHECK(colored[0].color.r == doctest::Approx(1.f));
    CHECK(colored[0].color.g == doctest::Approx(0.f));
    CHECK(colored[0].color.b == doctest::Approx(0.f));

    oscilline::Camera camera;
    camera.mode = oscilline::Camera::Mode::Orthographic;
    camera.width = 100;
    camera.height = 100;
    camera.ortho_pixels_per_unit = 1;
    const auto screen = oscilline::project_segments(colored, camera);
    REQUIRE(screen.size() == 1);
    CHECK(screen[0].color.r == doctest::Approx(1.f));
    CHECK(screen[0].color.g == doctest::Approx(0.f));
}

TEST_CASE("a fitted model keeps its color and sits inside the rectangle") {
    oscilline::ModelSegment line;
    line.a = {0.f, 0.f, 0.f};
    line.b = {10.f, 0.f, 0.f};
    line.color = {0.2f, 0.4f, 0.6f, 1.f};
    oscilline::ScreenRect rect;
    rect.left = 0.f;
    rect.top = 0.f;
    rect.right = 20.f;
    rect.bottom = 10.f;
    const auto fitted =
        oscilline::fit_model_xy(std::span<const oscilline::ModelSegment>(&line, 1), rect);
    REQUIRE(fitted.size() == 1);
    CHECK(fitted[0].x0 == doctest::Approx(0.f));
    CHECK(fitted[0].x1 == doctest::Approx(20.f));
    CHECK(fitted[0].y0 == doctest::Approx(5.f));
    CHECK(fitted[0].y1 == doctest::Approx(5.f));
    CHECK(fitted[0].color.r == doctest::Approx(0.2f));
    CHECK(fitted[0].color.g == doctest::Approx(0.4f));
    CHECK(fitted[0].color.b == doctest::Approx(0.6f));
    CHECK(oscilline::fit_model_xy({}, rect).empty());
}

TEST_CASE("digit objects are 0 through 9 and the text renderer keeps a line color") {
    CHECK(oscilline::digit_object(0, 10) == 0);
    CHECK(oscilline::digit_object(9, 10) == 9);
    CHECK_FALSE(oscilline::digit_object(9, 9).has_value());
    CHECK_FALSE(oscilline::digit_object(10, 12).has_value());

    const Stroke strokes[] = {{0, 0, 4, 0}};
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = oscilline::VectorFont::from_model(parsed.value(), map);
    oscilline::FontOptions options;
    options.scale = 1.f;
    options.tracking = 0.f;
    const auto laid = font.layout("A", options);
    const oscilline::Rgb red{1.f, 0.f, 0.f, 1.f};
    const auto placed = font.place(laid, 0.f, 0.f, options, red);
    REQUIRE(placed.size() == 1);
    CHECK(placed[0].color.r == doctest::Approx(1.f));
    CHECK(placed[0].color.g == doctest::Approx(0.f));
    CHECK(placed[0].color.b == doctest::Approx(0.f));
}
