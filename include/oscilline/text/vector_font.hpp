// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Draws the disc TMD font as strokes. Y grows down the screen.

#pragma once

#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/text/glyphs.hpp"
#include "oscilline/tmd.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oscilline {

struct FontOptions {
    // Model units to logical pixels.
    float scale = 1.f;
    // Horizontal stretch, independent of cap height.
    float horizontal_scale = 1.f;
    // PAL line-font coordinates increase down the screen.
    bool model_y_down = false;
    // Extra gap after each glyph, in logical pixels, before kerning.
    float tracking = 1.f;
    // Per-glyph positional shake, in logical pixels. 0 keeps the glyph still.
    // Non-zero values use ribbon_jitter_offset, the stage's 50 Hz uniform step.
    float jitter_x = 0.f;
    float jitter_y = 0.f;
    std::int64_t jitter_time_ms = 0;
    std::uint32_t jitter_salt = 0xD15Cu;
    // Extra pixels between baselines.
    float line_gap = 2.f;
    float line_width = 1.25f;
};

struct PlacedGlyph {
    bool missing = false;
    // Original blocky percent. The disc table has no '%' object.
    bool percent = false;
    bool slash = false;
    bool colon = false;
    // Synthetic vector plus sign; the disc table has no '+' object.
    bool plus = false;
    std::uint16_t object = 0;
    // Ink left, and the top of this line, both relative to the block.
    float x = 0.f;
    float y = 0.f;
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    float width = 0.f;
    float height = 0.f;
    int lines = 0;
};

// Line font. Advance and kerning come from each glyph's bounding box.
// A code point with no object draws a box, except for the original vector
// glyphs for '%', '/', ':', and '+'.
class VectorFont {
  public:
    [[nodiscard]] static VectorFont from_model(const TmdModel& model,
                                               std::span<const GlyphIndex> map);

    [[nodiscard]] float cap_height() const { return cap_height_; }
    [[nodiscard]] float em_width() const { return em_width_; }

    // `max_width` of 0 keeps the string on one line. Newlines always break.
    [[nodiscard]] TextLayout
    layout(std::string_view utf8, const FontOptions& options, float max_width = 0.f) const;

    // Screen segments. `x` and `y` are the top-left, matching debug text.
    // Model +Y is up unless model_y_down is set. The cap sits at `y`;
    // all glyphs share its reference, preserving descenders and accents.
    [[nodiscard]] std::vector<Segment> place(const TextLayout& layout,
                                             float x,
                                             float y,
                                             const FontOptions& options,
                                             Rgb color = {}) const;

  private:
    struct Shape {
        bool present = false;
        float min_x = 0.f;
        float min_y = 0.f;
        float max_x = 0.f;
        float max_y = 0.f;
        float ink = 0.f;
        float left_slack = 0.f;
        float right_slack = 0.f;
        std::vector<ModelSegment> lines;
    };

    std::vector<GlyphIndex> map_;
    std::vector<Shape> shapes_;
    float cap_height_ = 0.f;
    float baseline_y_ = 0.f;
    float em_width_ = 0.f;

    struct Hit {
        const Shape* shape = nullptr;
        std::uint16_t object = 0;
    };

    [[nodiscard]] Hit find_glyph(char32_t codepoint) const;
    [[nodiscard]] float kern(const Shape* previous, const Shape& current, float scale) const;
};

} // namespace oscilline
