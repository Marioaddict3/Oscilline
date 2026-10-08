// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Text painter interface for screens that share the font slot.

#pragma once

#include "oscilline/asset/registry.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/text/vector_font.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

struct TextStyle {
    // 0 keeps the string on one line. A positive width wraps on spaces.
    float max_width = 0.f;
    // Disc text uses this. Debug text stays the SDL color.
    Rgb color{};
};

struct TextTarget {
    std::vector<TextGlyph>* glyphs = nullptr;
    TriangleList* triangles = nullptr;
};

// Title, menus, loading, pause, and results draw through this. The debug
// implementation is the SDL text the window already used.
// Disc-font tracking, in logical pixels after each glyph. The vector-font
// default is 1; the disc painter opens the gap a little further.
inline constexpr float kDiscFontTracking = 2.5f;

// Disc-font positional shake, in PlayStation pixels of the 512×286 frame.
// Stage rest jitter is kRibbonJitterRestPs (1 PS px). This reuses
// ribbon_jitter_offset — the same 50 Hz uniform step — at a much smaller
// amplitude. The painter stretches it with kPlayStationPixelToLogicalX / Y.
inline constexpr float kDiscFontJitterPs = 0.5f;

// Cap height the disc painter targets, and the menu arrow's height.
inline constexpr float kDiscFontCapPx = 12.f;
inline constexpr float kDiscFontStrokeWidth = 1.25f;
inline constexpr float kDiscFontStrokeFeather = 0.5f;

class TextPainter {
  public:
    virtual ~TextPainter() = default;
    virtual void
    line(TextTarget& target, float x, float y, std::string value, TextStyle style = {}) const = 0;

    // Approximate SDL debug text width; vector-font painters provide their layout width.
    [[nodiscard]] virtual float measure_width(std::string_view value) const {
        return static_cast<float>(value.size()) * 8.f;
    }

    // Course or menu clock for the disc-font shake. Debug text ignores it.
    virtual void set_time_ms(std::int64_t /*time_ms*/) const {}

    // 0 is still. 1 keeps kDiscFontJitterPs. Options 2× is that amplitude
    // (the saved 1× is half of it). Debug text ignores it.
    virtual void set_jitter_scale(float /*scale*/) const {}
};

class DebugTextPainter final : public TextPainter {
  public:
    void line(TextTarget& target,
              float x,
              float y,
              std::string value,
              TextStyle style = {}) const override;
};

// Disc line font, scaled so a cap-height sits in the same band as debug text.
// Strokes go into `target.triangles`. Debug text is left untouched.
class DiscTextPainter final : public TextPainter {
  public:
    [[nodiscard]] static DiscTextPainter from_model(const TmdModel& model);

    void line(TextTarget& target,
              float x,
              float y,
              std::string value,
              TextStyle style = {}) const override;

    [[nodiscard]] float measure_width(std::string_view value) const override;

    void set_time_ms(std::int64_t time_ms) const override;

    void set_jitter_scale(float scale) const override;

  private:
    explicit DiscTextPainter(VectorFont font);

    VectorFont font_;
    FontOptions options_{};
    mutable std::int64_t time_ms_ = 0;
    // 1 keeps the amplitudes stored in options_.
    mutable float jitter_scale_ = 1.f;
};

// Right-pointing vector arrow in the disc stroke style. `x` and `y` are the
// top-left of the menu row; the mark sits in the leading indent.
void paint_menu_selector(TriangleList& triangles, float x, float y);

// Empty when the font slot is a placeholder or the model did not parse.
[[nodiscard]] std::optional<DiscTextPainter> font_painter_for(AssetRegistry& assets);

void paint_debug_line(std::vector<TextGlyph>& text, float x, float y, std::string value);

} // namespace oscilline
