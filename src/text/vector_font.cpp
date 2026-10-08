// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Lays out disc-font glyphs. Unmapped characters draw a box.

#include "oscilline/text/vector_font.hpp"

#include "oscilline/course/jitter.hpp"
#include "oscilline/render/project.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oscilline {
namespace {

constexpr float kMinAdvance = 4.f;

void append_slash(
    std::vector<Segment>& segments, float left, float top, float width, float height, Rgb color) {
    Segment segment;
    segment.x0 = left + width * 0.12f;
    segment.y0 = top + height * 0.94f;
    segment.x1 = left + width * 0.88f;
    segment.y1 = top + height * 0.06f;
    segment.color = color;
    segments.push_back(segment);
}

void append_percent(
    std::vector<Segment>& segments, float left, float top, float width, float height, Rgb color) {
    const float w = std::max(width, 1.f);
    const float h = std::max(height, 1.f);
    const auto edge = [&](float x0, float y0, float x1, float y1) {
        Segment segment;
        segment.x0 = left + x0 * w;
        segment.y0 = top + y0 * h;
        segment.x1 = left + x1 * w;
        segment.y1 = top + y1 * h;
        segment.color = color;
        segments.push_back(segment);
    };
    const auto box = [&](float x0, float y0, float x1, float y1) {
        edge(x0, y0, x1, y0);
        edge(x1, y0, x1, y1);
        edge(x1, y1, x0, y1);
        edge(x0, y1, x0, y0);
    };
    // Two block counters and a slash. Fractions of the cap box. Y grows down.
    box(0.08f, 0.06f, 0.40f, 0.38f);
    box(0.60f, 0.62f, 0.92f, 0.94f);
    edge(0.86f, 0.08f, 0.14f, 0.92f);
}

float glyph_jitter(const FontOptions& options, std::size_t index, bool vertical) {
    const std::uint32_t channel =
        options.jitter_salt ^ static_cast<std::uint32_t>(index + 1u) * 0x9E3779B9u;
    if (vertical) {
        return ribbon_jitter_offset(options.jitter_time_ms, channel ^ 0xB22u, options.jitter_y);
    }
    return ribbon_jitter_offset(options.jitter_time_ms, channel ^ 0xA11u, options.jitter_x);
}

std::vector<char32_t> decode_utf8(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        int need = 1;
        char32_t code = byte;
        if (byte < 0x80) {
            need = 1;
        } else if ((byte & 0xE0) == 0xC0 && i + 1 < text.size()) {
            need = 2;
            code = byte & 0x1F;
        } else if ((byte & 0xF0) == 0xE0 && i + 2 < text.size()) {
            need = 3;
            code = byte & 0x0F;
        } else if ((byte & 0xF8) == 0xF0 && i + 3 < text.size()) {
            need = 4;
            code = byte & 0x07;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool ok = need == 1;
        if (need > 1) {
            ok = true;
            for (int extra = 1; extra < need; ++extra) {
                const auto next =
                    static_cast<unsigned char>(text[i + static_cast<std::size_t>(extra)]);
                if ((next & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                code = (code << 6) | (next & 0x3F);
            }
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(code);
        i += static_cast<std::size_t>(need);
    }
    return out;
}

struct Bounds {
    bool any = false;
    float min_x = 0.f;
    float min_y = 0.f;
    float max_x = 0.f;
    float max_y = 0.f;
    std::vector<ModelSegment> lines;
};

void add_point(Bounds& bounds, float x, float y) {
    if (!bounds.any) {
        bounds.min_x = bounds.max_x = x;
        bounds.min_y = bounds.max_y = y;
        bounds.any = true;
        return;
    }
    bounds.min_x = std::min(bounds.min_x, x);
    bounds.min_y = std::min(bounds.min_y, y);
    bounds.max_x = std::max(bounds.max_x, x);
    bounds.max_y = std::max(bounds.max_y, y);
}

Bounds bounds_for_object(const TmdModel& model, std::size_t object_index) {
    Bounds bounds;
    if (object_index >= model.objects.size()) {
        return bounds;
    }
    std::vector<Pose> poses(model.objects.size());
    poses[object_index].visible = true;
    bounds.lines = tmd_wireframe(model, poses);
    for (const ModelSegment& line : bounds.lines) {
        add_point(bounds, line.a.x, line.a.y);
        add_point(bounds, line.b.x, line.b.y);
    }
    return bounds;
}

} // namespace

VectorFont VectorFont::from_model(const TmdModel& model, std::span<const GlyphIndex> map) {
    VectorFont font;
    font.map_.assign(map.begin(), map.end());
    std::sort(font.map_.begin(), font.map_.end(), [](const GlyphIndex& a, const GlyphIndex& b) {
        return a.codepoint < b.codepoint;
    });
    font.shapes_.resize(model.objects.size());
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        const Bounds bounds = bounds_for_object(model, index);
        Shape& shape = font.shapes_[index];
        if (!bounds.any) {
            continue;
        }
        shape.present = true;
        shape.min_x = bounds.min_x;
        shape.min_y = bounds.min_y;
        shape.max_x = bounds.max_x;
        shape.max_y = bounds.max_y;
        shape.ink = std::max(0.f, bounds.max_x - bounds.min_x);
        shape.left_slack = std::max(0.f, bounds.min_x);
        shape.lines = bounds.lines;
        font.em_width_ = std::max(font.em_width_, shape.ink);
    }
    for (Shape& shape : font.shapes_) {
        if (!shape.present) {
            continue;
        }
        shape.right_slack = std::max(0.f, font.em_width_ - shape.ink);
    }
    const auto cap_glyph =
        std::find_if(font.map_.begin(), font.map_.end(), [&font](const GlyphIndex& row) {
            return row.codepoint == U'A' && row.object < font.shapes_.size() &&
                   font.shapes_[row.object].present;
        });
    if (cap_glyph != font.map_.end()) {
        const Shape& cap = font.shapes_[cap_glyph->object];
        font.cap_height_ = cap.max_y - cap.min_y;
        font.baseline_y_ = cap.min_y;
    } else {
        for (const Shape& shape : font.shapes_) {
            if (shape.present && shape.max_y - shape.min_y > font.cap_height_) {
                font.cap_height_ = shape.max_y - shape.min_y;
                font.baseline_y_ = shape.min_y;
            }
        }
    }
    return font;
}

VectorFont::Hit VectorFont::find_glyph(char32_t codepoint) const {
    const auto found = std::lower_bound(
        map_.begin(), map_.end(), codepoint, [](const GlyphIndex& row, char32_t code) {
            return row.codepoint < code;
        });
    if (found == map_.end() || found->codepoint != codepoint) {
        return {};
    }
    if (found->object >= shapes_.size()) {
        return {};
    }
    const Shape& shape = shapes_[found->object];
    if (!shape.present) {
        return {};
    }
    Hit hit;
    hit.shape = &shape;
    hit.object = found->object;
    return hit;
}

float VectorFont::kern(const Shape* previous, const Shape& current, float scale) const {
    if (previous == nullptr) {
        return 0.f;
    }
    const float slack = std::min(previous->right_slack, current.left_slack);
    if (!(slack > 0.f)) {
        return 0.f;
    }
    return 0.5f * slack * scale;
}

TextLayout
VectorFont::layout(std::string_view utf8, const FontOptions& options, float max_width) const {
    TextLayout layout;
    const float scale = std::isfinite(options.scale) && options.scale > 0.f ? options.scale : 1.f;
    const float horizontal_scale =
        std::isfinite(options.horizontal_scale) && options.horizontal_scale > 0.f
            ? options.horizontal_scale
            : 1.f;
    const float x_scale = scale * horizontal_scale;
    const float tracking =
        std::isfinite(options.tracking) && options.tracking > 0.f ? options.tracking : 0.f;
    const float line_gap =
        std::isfinite(options.line_gap) && options.line_gap > 0.f ? options.line_gap : 0.f;
    // A flat glyph still occupies a row so the next line does not sit on it.
    const float cap = std::max(cap_height_ * scale, 1.f);
    const float stride = cap + line_gap;
    const float missing_ink = std::max(em_width_, kMinAdvance);
    const float space_advance = missing_ink * x_scale;

    const std::vector<char32_t> codes = decode_utf8(utf8);
    float pen = 0.f;
    int line = 0;
    float block_width = 0.f;
    const Shape* previous = nullptr;
    bool line_has_ink = false;

    const auto break_line = [&] {
        pen = 0.f;
        previous = nullptr;
        line_has_ink = false;
        ++line;
    };

    for (char32_t code : codes) {
        if (code == U'\n') {
            break_line();
            continue;
        }
        // Whitespace never has visible ink, even if a font table assigns its
        // code point to an object used as a spacer or fallback box.
        if (code == U' ') {
            if (max_width > 0.f && line_has_ink && (pen + space_advance) > max_width) {
                break_line();
                continue;
            }
            pen += space_advance;
            block_width = std::max(block_width, pen);
            previous = nullptr;
            line_has_ink = true;
            continue;
        }
        const Hit hit = find_glyph(code);
        // The disc table has no percent. Draw the original block instead of a box.
        const bool percent = code == U'%' && hit.shape == nullptr;
        const bool slash = code == U'/' && hit.shape == nullptr;
        const bool colon = code == U':' && hit.shape == nullptr;
        const bool plus = code == U'+' && hit.shape == nullptr;
        const float slash_ink = std::max(em_width_ * 0.55f, kMinAdvance);
        const float colon_ink = std::max(em_width_ * 0.34f, kMinAdvance);
        const float plus_ink = std::max(em_width_ * 0.65f, kMinAdvance);
        const float ink = hit.shape != nullptr ? hit.shape->ink
                          : percent            ? std::max(em_width_, kMinAdvance)
                          : slash              ? slash_ink
                          : colon              ? colon_ink
                          : plus               ? plus_ink
                                               : missing_ink;
        const float advance = ink * x_scale;
        float use_kern =
            (hit.shape != nullptr && line_has_ink) ? kern(previous, *hit.shape, x_scale) : 0.f;
        if (max_width > 0.f && line_has_ink && (pen - use_kern + advance) > max_width) {
            break_line();
            use_kern = 0.f;
        }
        PlacedGlyph placed;
        placed.missing = hit.shape == nullptr && !percent && !slash && !colon && !plus;
        placed.percent = percent;
        placed.slash = slash;
        placed.colon = colon;
        placed.plus = plus;
        placed.object = hit.object;
        placed.x = pen - use_kern;
        placed.y = static_cast<float>(line) * stride;
        layout.glyphs.push_back(placed);
        pen = placed.x + advance + tracking;
        block_width = std::max(block_width, placed.x + advance);
        previous = percent ? nullptr : hit.shape;
        line_has_ink = true;
    }
    layout.width = block_width;
    if (!line_has_ink && line == 0 && layout.glyphs.empty()) {
        layout.lines = 0;
        layout.height = 0.f;
    } else {
        layout.lines = line + 1;
        layout.height = cap + static_cast<float>(line) * stride;
    }
    return layout;
}

std::vector<Segment> VectorFont::place(
    const TextLayout& layout, float x, float y, const FontOptions& options, Rgb color) const {
    std::vector<Segment> segments;
    const float scale = std::isfinite(options.scale) && options.scale > 0.f ? options.scale : 1.f;
    const float horizontal_scale =
        std::isfinite(options.horizontal_scale) && options.horizontal_scale > 0.f
            ? options.horizontal_scale
            : 1.f;
    const float x_scale = scale * horizontal_scale;
    // Zero cap stays put so a flat glyph matches an orthographic projection
    // of the same model point. The missing-glyph box still has a height.
    const float cap = std::max(cap_height_ * scale, 0.f);
    const float box_h = std::max(cap, 1.f);
    const float missing_ink = std::max(em_width_, kMinAdvance) * x_scale;
    for (std::size_t index = 0; index < layout.glyphs.size(); ++index) {
        const PlacedGlyph& glyph = layout.glyphs[index];
        const float left = x + glyph.x + glyph_jitter(options, index, false);
        const float top = y + glyph.y + glyph_jitter(options, index, true);
        if (glyph.percent) {
            append_percent(segments, left, top, missing_ink, box_h, color);
            continue;
        }
        if (glyph.slash) {
            const float slash_width = std::max(em_width_ * 0.55f, kMinAdvance) * x_scale;
            append_slash(segments, left, top, slash_width, box_h, color);
            continue;
        }
        if (glyph.plus) {
            const float width = std::max(em_width_ * 0.65f, kMinAdvance) * x_scale;
            const float center_x = left + width * 0.5f;
            const float center_y = top + box_h * 0.5f;
            const float half_width = width * 0.42f;
            const float half_height = box_h * 0.28f;
            Segment horizontal;
            horizontal.x0 = center_x - half_width;
            horizontal.y0 = center_y;
            horizontal.x1 = center_x + half_width;
            horizontal.y1 = center_y;
            horizontal.color = color;
            segments.push_back(horizontal);
            Segment vertical;
            vertical.x0 = center_x;
            vertical.y0 = center_y - half_height;
            vertical.x1 = center_x;
            vertical.y1 = center_y + half_height;
            vertical.color = color;
            segments.push_back(vertical);
            continue;
        }
        if (glyph.colon) {
            const float cx = left + missing_ink * 0.17f;
            const float radius = std::max(std::min(cap, missing_ink) * 0.10f, 0.8f);
            const float centers[] = {top + cap * 0.30f, top + cap * 0.72f};
            for (const float cy : centers) {
                const auto edge = [&](float x0, float y0, float x1, float y1) {
                    Segment segment;
                    segment.x0 = x0;
                    segment.y0 = y0;
                    segment.x1 = x1;
                    segment.y1 = y1;
                    segment.color = color;
                    segments.push_back(segment);
                };
                edge(cx, cy - radius, cx + radius, cy);
                edge(cx + radius, cy, cx, cy + radius);
                edge(cx, cy + radius, cx - radius, cy);
                edge(cx - radius, cy, cx, cy - radius);
            }
            continue;
        }
        if (glyph.missing || glyph.object >= shapes_.size() || !shapes_[glyph.object].present) {
            const float right = left + missing_ink;
            const float bottom = top + box_h;
            const auto edge = [&](float x0, float y0, float x1, float y1) {
                Segment segment;
                segment.x0 = x0;
                segment.y0 = y0;
                segment.x1 = x1;
                segment.y1 = y1;
                segment.color = color;
                segments.push_back(segment);
            };
            edge(left, top, right, top);
            edge(right, top, right, bottom);
            edge(right, bottom, left, bottom);
            edge(left, bottom, left, top);
            continue;
        }
        const Shape& shape = shapes_[glyph.object];
        const float baseline = top + cap + baseline_y_ * scale;
        const auto screen_y = [&](float model_y) {
            return options.model_y_down ? top + (model_y - baseline_y_) * scale
                                        : baseline - model_y * scale;
        };
        for (const ModelSegment& line : shape.lines) {
            Segment segment;
            segment.x0 = left + (line.a.x - shape.min_x) * x_scale;
            segment.y0 = screen_y(line.a.y);
            segment.x1 = left + (line.b.x - shape.min_x) * x_scale;
            segment.y1 = screen_y(line.b.y);
            segment.color = color;
            segments.push_back(segment);
        }
    }
    return segments;
}

} // namespace oscilline
