// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Glyph indexes and vector-font layout.

#include "oscilline/course/jitter.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/settings.hpp"
#include "oscilline/text/glyphs.hpp"
#include "oscilline/text/vector_font.hpp"
#include "oscilline/tmd.hpp"
#include "text_out.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <span>
#include <string>
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
};

// One line object per stroke. Offsets are relative to the object table.
Buf line_objects(std::span<const Stroke> strokes) {
    Buf file;
    const auto count = static_cast<std::uint32_t>(strokes.size());
    file.u32(0x41);
    file.u32(0);
    file.u32(count);
    const std::uint32_t table = count * 0x1Cu;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t vertex = table + index * 28u;
        file.u32(vertex);
        file.u32(2);
        file.u32(0);
        file.u32(0);
        file.u32(vertex + 16u);
        file.u32(1);
        file.i32(0);
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
        file.u8(3);
        file.u8(2);
        file.u8(1);
        file.u8(0x40);
        file.u32(0x00FFFFFF);
        file.u16(0);
        file.u16(1);
    }
    return file;
}

oscilline::VectorFont font_of(std::span<const Stroke> strokes,
                              std::span<const oscilline::GlyphIndex> map) {
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    return oscilline::VectorFont::from_model(parsed.value(), map);
}

oscilline::FontOptions flat_options() {
    oscilline::FontOptions options;
    options.scale = 1.f;
    options.tracking = 1.f;
    options.line_gap = 2.f;
    options.line_width = 1.f;
    return options;
}

oscilline::Camera ortho_camera() {
    oscilline::Camera camera;
    camera.mode = oscilline::Camera::Mode::Orthographic;
    camera.width = 100;
    camera.height = 100;
    camera.ortho_pixels_per_unit = 1;
    return camera;
}

} // namespace

TEST_CASE("the committed glyph table is an index map, not outlines") {
    CHECK(oscilline::font_glyphs().size() == 73);
    CHECK(oscilline::glyph_object(U' ') == 0);
    CHECK(oscilline::glyph_object(U'0') == 0);
    CHECK(oscilline::glyph_object(U'A') == 10);
    CHECK(oscilline::glyph_object(U'a') == 36);
    CHECK(oscilline::glyph_object(U'?') == 62);
    CHECK(oscilline::glyph_object(U'!') == 63);
    CHECK(oscilline::glyph_object(U'.') == 65);
    CHECK(oscilline::glyph_object(U',') == 66);
    CHECK(oscilline::glyph_object(U'\'') == 67);
    CHECK(oscilline::glyph_object(U'-') == 68);
    CHECK(oscilline::glyph_object(U'(') == 69);
    CHECK(oscilline::glyph_object(U')') == 70);
    CHECK_FALSE(oscilline::glyph_object(U'>').has_value());
    CHECK_FALSE(oscilline::glyph_object(U'%').has_value());
    CHECK(oscilline::glyph_object(U'~') == 108);
    CHECK_FALSE(oscilline::glyph_object(U'/').has_value());
    CHECK_FALSE(oscilline::glyph_object(U'\u00D1').has_value());
    CHECK_FALSE(oscilline::glyph_object(U'\u00E9').has_value());
}

TEST_CASE("layout width is the ink plus tracking and drops the trailing gap") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 0, 4, 0}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}, {U'B', 1}};
    const oscilline::VectorFont font = font_of(strokes, map);
    const oscilline::TextLayout layout = font.layout("AB", flat_options());
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.glyphs[0].x == doctest::Approx(0));
    CHECK(layout.glyphs[1].x == doctest::Approx(11));
    CHECK(layout.width == doctest::Approx(15));
    CHECK(layout.lines == 1);
}

TEST_CASE("kerning pulls a glyph left by half the shared slack") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 0, 6, 0}, {4, 0, 10, 0}};
    const oscilline::GlyphIndex map[] = {{U'W', 0}, {U'A', 1}, {U'B', 2}};
    const oscilline::VectorFont font = font_of(strokes, map);
    CHECK(font.em_width() == doctest::Approx(10));
    const oscilline::TextLayout layout = font.layout("AB", flat_options());
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.glyphs[0].x == doctest::Approx(0));
    CHECK(layout.glyphs[1].x == doctest::Approx(5));
    CHECK(layout.width == doctest::Approx(11));
}

TEST_CASE("font horizontal stretch changes advance without changing cap height") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.horizontal_scale = 2.f;

    const oscilline::TextLayout layout = font.layout("AA", options);
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.glyphs[1].x == doctest::Approx(21));
    CHECK(layout.width == doctest::Approx(41));
    CHECK(layout.height == doctest::Approx(8));
    const auto placed = font.place(layout, 0.f, 0.f, options);
    REQUIRE(placed.size() == 2);
    CHECK(placed[0].x1 == doctest::Approx(20));
    CHECK(placed[1].x1 == doctest::Approx(41));
}

TEST_CASE("wrapping measures the kerned width and resets kerning on a new line") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 0, 6, 0}, {4, 0, 10, 0}};
    const oscilline::GlyphIndex map[] = {{U'W', 0}, {U'A', 1}, {U'B', 2}};
    const oscilline::VectorFont font = font_of(strokes, map);
    const oscilline::TextLayout fits = font.layout("AB", flat_options(), 11.f);
    REQUIRE(fits.glyphs.size() == 2);
    CHECK(fits.lines == 1);
    CHECK(fits.width == doctest::Approx(11));
    CHECK(fits.glyphs[1].x == doctest::Approx(5));

    const oscilline::TextLayout wraps = font.layout("AB", flat_options(), 10.f);
    REQUIRE(wraps.glyphs.size() == 2);
    CHECK(wraps.lines == 2);
    CHECK(wraps.glyphs[1].x == doctest::Approx(0));
}

TEST_CASE("centered glyph geometry is placed against the font cap baseline") {
    const Stroke strokes[] = {{0, -4, 10, 4}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();

    const oscilline::TextLayout layout = font.layout("A", options);
    CHECK(layout.height == doctest::Approx(8));
    const auto placed = font.place(layout, 0.f, 0.f, options);
    REQUIRE(placed.size() == 1);
    CHECK(std::min(placed[0].y0, placed[0].y1) == doctest::Approx(0));
    CHECK(std::max(placed[0].y0, placed[0].y1) == doctest::Approx(8));
}

TEST_CASE("screen-down font coordinates keep caps upright and descenders below them") {
    const Stroke strokes[] = {{0, -4, 10, 4}, {0, 0, 6, 7}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}, {U'g', 1}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.model_y_down = true;
    const auto laid = font.layout("Ag", options);
    const auto placed = font.place(laid, 20.f, 30.f, options);
    REQUIRE(placed.size() == 2);
    CHECK(laid.height == doctest::Approx(8));
    CHECK(placed[0].y0 == doctest::Approx(30));
    CHECK(placed[0].y1 == doctest::Approx(38));
    CHECK(placed[1].y0 == doctest::Approx(34));
    CHECK(placed[1].y1 == doctest::Approx(41));
}

TEST_CASE("a missing code point draws a box and an accented letter uses the map") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'\u00E9', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;

    const oscilline::TextLayout missing = font.layout("?", options);
    REQUIRE(missing.glyphs.size() == 1);
    CHECK(missing.glyphs[0].missing);
    const auto box = font.place(missing, 0.f, 0.f, options);
    CHECK(box.size() == 4);

    const std::string accented = "\u00E9";
    const oscilline::TextLayout accent = font.layout(accented, options);
    REQUIRE(accent.glyphs.size() == 1);
    CHECK_FALSE(accent.glyphs[0].missing);
    CHECK(accent.glyphs[0].object == 0);
    const auto ink = font.place(accent, 0.f, 0.f, options);
    REQUIRE(ink.size() == 1);
    CHECK(ink[0].x0 == doctest::Approx(0));
    CHECK(ink[0].y0 == doctest::Approx(8));
    CHECK(ink[0].x1 == doctest::Approx(10));
    CHECK(ink[0].y1 == doctest::Approx(0));
}

TEST_CASE("a line wraps when the next glyph does not fit") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.line_gap = 2.f;
    const oscilline::TextLayout layout = font.layout("AA", options, 12.f);
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.lines == 2);
    CHECK(layout.glyphs[0].y == doctest::Approx(0));
    CHECK(layout.glyphs[1].x == doctest::Approx(0));
    CHECK(layout.glyphs[1].y == doctest::Approx(10));
}

TEST_CASE("placing a flat glyph matches an orthographic projection") {
    const Stroke strokes[] = {{0, 0, 10, 0}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    const oscilline::VectorFont font = oscilline::VectorFont::from_model(parsed.value(), map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;
    const oscilline::TextLayout layout = font.layout("A", options);
    const auto placed = font.place(layout, 50.f, 50.f, options);
    const auto projected =
        oscilline::project_segments(oscilline::tmd_wireframe(parsed.value()), ortho_camera());
    REQUIRE(placed.size() == 1);
    REQUIRE(projected.size() == 1);
    CHECK(placed[0].x0 == doctest::Approx(projected[0].x0));
    CHECK(placed[0].y0 == doctest::Approx(projected[0].y0));
    CHECK(placed[0].x1 == doctest::Approx(projected[0].x1));
    CHECK(placed[0].y1 == doctest::Approx(projected[0].y1));
    CHECK(placed[0].x0 == doctest::Approx(50));
    CHECK(placed[0].y0 == doctest::Approx(50));
    CHECK(placed[0].x1 == doctest::Approx(60));
    CHECK(placed[0].y1 == doctest::Approx(50));
}

TEST_CASE("leading spaces indent and a wrapped space is not drawn") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 0, 10, 8}};
    // Even if the font table maps space to a visible fallback object, it must
    // still lay out as whitespace and never paint that object's outline.
    const oscilline::GlyphIndex map[] = {{U' ', 1}, {U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;
    const oscilline::TextLayout indented = font.layout("  A", options);
    REQUIRE(indented.glyphs.size() == 1);
    CHECK_FALSE(indented.glyphs[0].missing);
    CHECK(indented.glyphs[0].x == doctest::Approx(20));

    const oscilline::TextLayout wrapped = font.layout("A A", options, 16.f);
    REQUIRE(wrapped.lines == 2);
    REQUIRE(wrapped.glyphs.size() == 2);
    CHECK(wrapped.glyphs[1].x == doctest::Approx(0));
}

TEST_CASE("percent is a blocky vector glyph and not the missing box") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 1.f;

    const oscilline::TextLayout layout = font.layout("A%", options);
    REQUIRE(layout.glyphs.size() == 2);
    CHECK_FALSE(layout.glyphs[0].percent);
    CHECK(layout.glyphs[1].percent);
    CHECK_FALSE(layout.glyphs[1].missing);
    CHECK(layout.glyphs[1].x == doctest::Approx(11));
    CHECK(layout.width == doctest::Approx(21));

    const auto placed = font.place(font.layout("%", options), 0.f, 0.f, options);
    CHECK(placed.size() == 9);
    int diagonals = 0;
    float top_box = 1.0e9f;
    float bottom_box = -1.0e9f;
    for (const oscilline::Segment& segment : placed) {
        const bool axis =
            segment.x0 == doctest::Approx(segment.x1) || segment.y0 == doctest::Approx(segment.y1);
        if (axis) {
            top_box = std::min(top_box, std::min(segment.y0, segment.y1));
            bottom_box = std::max(bottom_box, std::max(segment.y0, segment.y1));
            continue;
        }
        ++diagonals;
        const bool first_higher = segment.y0 < segment.y1;
        const float upper_x = first_higher ? segment.x0 : segment.x1;
        const float lower_x = first_higher ? segment.x1 : segment.x0;
        CHECK(upper_x > lower_x);
    }
    CHECK(diagonals == 1);
    CHECK(top_box < bottom_box);
    CHECK(top_box == doctest::Approx(0.48f));
    CHECK(bottom_box == doctest::Approx(7.52f));
}

TEST_CASE("glyph shake reuses the stage jitter step and stays inside its amplitude") {
    const Stroke strokes[] = {{0, 0, 10, 8}, {0, 0, 6, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}, {U'B', 1}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions still = flat_options();
    still.tracking = 0.f;
    still.jitter_x = 0.f;
    still.jitter_y = 0.f;
    const auto rest = font.place(font.layout("AB", still), 20.f, 30.f, still);
    REQUIRE(rest.size() == 2);

    oscilline::FontOptions shaken = still;
    shaken.jitter_x = 2.f;
    shaken.jitter_y = 1.5f;
    bool moved = false;
    bool glyphs_differ = false;
    for (std::int64_t time_ms : {0, 20, 40, 80, 200, 1000}) {
        shaken.jitter_time_ms = time_ms;
        const auto placed = font.place(font.layout("AB", shaken), 20.f, 30.f, shaken);
        REQUIRE(placed.size() == rest.size());
        const float dx0 = placed[0].x0 - rest[0].x0;
        const float dy0 = placed[0].y0 - rest[0].y0;
        const float dx1 = placed[1].x0 - rest[1].x0;
        const float dy1 = placed[1].y0 - rest[1].y0;
        CHECK(std::fabs(dx0) <= 2.f + 1.e-4f);
        CHECK(std::fabs(dy0) <= 1.5f + 1.e-4f);
        CHECK(std::fabs(dx1) <= 2.f + 1.e-4f);
        CHECK(std::fabs(dy1) <= 1.5f + 1.e-4f);
        CHECK(placed[0].x1 - placed[0].x0 == doctest::Approx(rest[0].x1 - rest[0].x0));
        CHECK(placed[0].y1 - placed[0].y0 == doctest::Approx(rest[0].y1 - rest[0].y0));
        if (dx0 != 0.f || dy0 != 0.f) {
            moved = true;
        }
        if (dx0 != dx1 || dy0 != dy1) {
            glyphs_differ = true;
        }
    }
    CHECK(moved);
    CHECK(glyphs_differ);

    CHECK(oscilline::kDiscFontTracking == doctest::Approx(2.5f));
    CHECK(oscilline::kDiscFontJitterPs > 0.f);
    CHECK(oscilline::kDiscFontJitterPs < oscilline::kRibbonJitterRestPs);
}

TEST_CASE("disc font tracking is a half pixel wider than the previous gap") {
    const Stroke strokes[] = {{0, 0, 10, 0}, {0, 0, 4, 0}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}, {U'B', 1}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions previous = flat_options();
    previous.tracking = 2.f;
    oscilline::FontOptions opened = previous;
    opened.tracking = oscilline::kDiscFontTracking;
    const oscilline::TextLayout before = font.layout("AB", previous);
    const oscilline::TextLayout after = font.layout("AB", opened);
    CHECK(after.glyphs[1].x - before.glyphs[1].x == doctest::Approx(0.5f));
    CHECK(after.width - before.width == doctest::Approx(0.5f));
}

TEST_CASE("disc text shake is still at zero, half the built-in amplitude at 1x, and that "
          "amplitude at 2x") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    auto parsed = oscilline::parse_tmd(line_objects(strokes).bytes);
    REQUIRE(parsed);
    oscilline::DiscTextPainter painter = oscilline::DiscTextPainter::from_model(parsed.value());
    const auto draw = [&](float scale, std::int64_t time_ms) {
        painter.set_jitter_scale(scale);
        painter.set_time_ms(time_ms);
        oscilline::TriangleList triangles;
        oscilline::TextTarget target;
        target.triangles = &triangles;
        painter.line(target, 40.f, 80.f, "0");
        REQUIRE_FALSE(triangles.vertices.empty());
        return triangles.vertices.front();
    };
    const auto still = draw(0.f, 0);
    const float base_x = oscilline::kDiscFontJitterPs * oscilline::kPlayStationPixelToLogicalX;
    const float base_y = oscilline::kDiscFontJitterPs * oscilline::kPlayStationPixelToLogicalY;
    bool moved = false;
    for (const std::int64_t time_ms : {0, 20, 40, 80, 200, 1000}) {
        const auto off = draw(0.f, time_ms);
        CHECK(off.x == doctest::Approx(still.x));
        CHECK(off.y == doctest::Approx(still.y));
        const auto once = draw(oscilline::text_shake_scale(oscilline::kTextShakeDefault), time_ms);
        const auto twice = draw(oscilline::text_shake_scale(oscilline::kTextShakeMax), time_ms);
        const float dx = once.x - still.x;
        const float dy = once.y - still.y;
        CHECK(std::fabs(dx) <= base_x * 0.5f + 1.e-3f);
        CHECK(std::fabs(dy) <= base_y * 0.5f + 1.e-3f);
        CHECK(twice.x - still.x == doctest::Approx(dx * 2.f));
        CHECK(twice.y - still.y == doctest::Approx(dy * 2.f));
        const auto clamped = draw(9.f, time_ms);
        CHECK(clamped.x == doctest::Approx(twice.x));
        CHECK(clamped.y == doctest::Approx(twice.y));
        if (dx != 0.f || dy != 0.f) {
            moved = true;
        }
    }
    CHECK(moved);
}

TEST_CASE("the menu selector is a right-pointing vector arrow") {
    oscilline::TriangleList triangles;
    oscilline::paint_menu_selector(triangles, 40.f, 80.f);
    REQUIRE(triangles.vertices.size() >= 9);
    float min_x = triangles.vertices[0].x;
    float max_x = min_x;
    float min_y = triangles.vertices[0].y;
    float max_y = min_y;
    for (const oscilline::Vertex& vertex : triangles.vertices) {
        min_x = std::min(min_x, vertex.x);
        max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y);
        max_y = std::max(max_y, vertex.y);
    }
    CHECK(min_x > 38.f);
    CHECK(max_x > min_x + 8.f);
    CHECK(max_x < 40.f + oscilline::kDiscFontCapPx + 2.f);
    CHECK(min_y > 78.f);
    CHECK(max_y < 80.f + oscilline::kDiscFontCapPx + 2.f);
    CHECK(max_y > min_y + 4.f);
}

TEST_CASE("slash is drawn as a narrow ascending vector stroke") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;

    const oscilline::TextLayout layout = font.layout("/", options);
    REQUIRE(layout.glyphs.size() == 1);
    CHECK(layout.glyphs[0].slash);
    CHECK_FALSE(layout.glyphs[0].missing);
    CHECK(layout.width < 7.f);
    const auto ink = font.place(layout, 0.f, 0.f, options);
    REQUIRE(ink.size() == 1);
    CHECK(ink[0].x0 < ink[0].x1);
    CHECK(ink[0].y0 > ink[0].y1);
}

TEST_CASE("colon is drawn as a narrow disc-style pair of outlined diamonds") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;

    const oscilline::TextLayout layout = font.layout(":", options);
    REQUIRE(layout.glyphs.size() == 1);
    CHECK(layout.glyphs[0].colon);
    CHECK_FALSE(layout.glyphs[0].missing);
    CHECK(layout.width < 6.f);
    const auto ink = font.place(layout, 0.f, 0.f, options);
    CHECK(ink.size() == 8);
    for (const oscilline::Segment& segment : ink) {
        CHECK(segment.x0 >= 0.f);
        CHECK(segment.x1 >= 0.f);
        CHECK(segment.y0 >= 0.f);
        CHECK(segment.y1 <= layout.height);
    }
}

TEST_CASE("plus is drawn as a vector cross instead of the missing-glyph box") {
    const Stroke strokes[] = {{0, 0, 10, 8}};
    const oscilline::GlyphIndex map[] = {{U'A', 0}};
    const oscilline::VectorFont font = font_of(strokes, map);
    oscilline::FontOptions options = flat_options();
    options.tracking = 0.f;

    const oscilline::TextLayout layout = font.layout("+", options);
    REQUIRE(layout.glyphs.size() == 1);
    CHECK(layout.glyphs[0].plus);
    CHECK_FALSE(layout.glyphs[0].missing);
    const auto ink = font.place(layout, 0.f, 0.f, options);
    REQUIRE(ink.size() == 2);
    CHECK(ink[0].y0 == doctest::Approx(ink[0].y1));
    CHECK(ink[1].x0 == doctest::Approx(ink[1].x1));
    CHECK(ink[0].x0 < ink[0].x1);
    CHECK(ink[1].y0 < ink[1].y1);
}
