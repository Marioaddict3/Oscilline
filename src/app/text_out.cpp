// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Draws title, menu, and results strings from the disc font or SDL text.

#include "text_out.hpp"

#include "oscilline/asset/map.hpp"
#include "oscilline/course/jitter.hpp"
#include "oscilline/settings.hpp"
#include "oscilline/text/glyphs.hpp"

#include <cmath>
#include <vector>

namespace oscilline {

void DebugTextPainter::line(
    TextTarget& target, float x, float y, std::string value, TextStyle /*style*/) const {
    if (target.glyphs == nullptr || value.empty()) {
        return;
    }
    TextGlyph item;
    item.x = x;
    item.y = y;
    item.text = std::move(value);
    target.glyphs->push_back(std::move(item));
}

DiscTextPainter::DiscTextPainter(VectorFont font) : font_(std::move(font)) {
    const float cap = font_.cap_height();
    options_.scale = cap > 0.01f ? kDiscFontCapPx / cap : 1.f;
    options_.horizontal_scale = 2.f;
    options_.model_y_down = true;
    options_.tracking = kDiscFontTracking;
    options_.jitter_x = kDiscFontJitterPs * kPlayStationPixelToLogicalX;
    options_.jitter_y = kDiscFontJitterPs * kPlayStationPixelToLogicalY;
    options_.line_gap = 2.f;
    options_.line_width = kDiscFontStrokeWidth;
}

DiscTextPainter DiscTextPainter::from_model(const TmdModel& model) {
    return DiscTextPainter(VectorFont::from_model(model, font_glyphs()));
}

float DiscTextPainter::measure_width(std::string_view value) const {
    return font_.layout(value, options_).width;
}

void DiscTextPainter::set_time_ms(std::int64_t time_ms) const {
    time_ms_ = time_ms;
}

void DiscTextPainter::set_jitter_scale(float scale) const {
    if (!std::isfinite(scale)) {
        scale = 0.f;
    }
    const float max_scale = text_shake_scale(kTextShakeMax);
    if (scale < 0.f) {
        scale = 0.f;
    } else if (scale > max_scale) {
        scale = max_scale;
    }
    jitter_scale_ = scale;
}

void DiscTextPainter::line(
    TextTarget& target, float x, float y, std::string value, TextStyle style) const {
    if (target.triangles == nullptr || value.empty()) {
        return;
    }
    FontOptions options = options_;
    options.jitter_time_ms = time_ms_;
    options.jitter_x *= jitter_scale_;
    options.jitter_y *= jitter_scale_;
    const TextLayout laid = font_.layout(value, options, style.max_width);
    const std::vector<Segment> segments = font_.place(laid, x, y, options, style.color);
    StrokeStyle stroke;
    stroke.width = options_.line_width;
    stroke.feather = kDiscFontStrokeFeather;
    append_strokes(*target.triangles, segments, stroke);
}

void paint_menu_selector(TriangleList& triangles, float x, float y) {
    const float mid = y + kDiscFontCapPx * 0.50f;
    const float top = y + kDiscFontCapPx * 0.18f;
    const float bot = y + kDiscFontCapPx * 0.82f;
    const float tail = x + 0.5f;
    const float neck = x + kDiscFontCapPx * 0.42f;
    const float tip = x + kDiscFontCapPx * 0.92f;
    std::vector<Segment> segments;
    const auto add = [&](float x0, float y0, float x1, float y1) {
        Segment segment;
        segment.x0 = x0;
        segment.y0 = y0;
        segment.x1 = x1;
        segment.y1 = y1;
        segments.push_back(segment);
    };
    // Shaft, then the two edges of a head that points right.
    add(tail, mid, neck, mid);
    add(neck, top, tip, mid);
    add(tip, mid, neck, bot);
    StrokeStyle stroke;
    stroke.width = kDiscFontStrokeWidth;
    stroke.feather = kDiscFontStrokeFeather;
    append_strokes(triangles, segments, stroke);
}

std::optional<DiscTextPainter> font_painter_for(AssetRegistry& assets) {
    if (assets.origin(Slot::Font) != AssetOrigin::Disc) {
        return std::nullopt;
    }
    const TmdModel* model = assets.model(Slot::Font);
    if (model == nullptr) {
        return std::nullopt;
    }
    return DiscTextPainter::from_model(*model);
}

void paint_debug_line(std::vector<TextGlyph>& text, float x, float y, std::string value) {
    DebugTextPainter painter;
    TextTarget target;
    target.glyphs = &text;
    painter.line(target, x, y, std::move(value));
}

} // namespace oscilline
