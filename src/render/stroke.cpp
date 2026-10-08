// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Core plus feather quads, clipped to the logical view.

#include "oscilline/render/stroke.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace oscilline {
namespace {

constexpr float kMinLength = 0.001f;

Vertex make_vertex(float x, float y, const Rgb& color, float alpha) {
    Vertex vertex;
    vertex.x = x;
    vertex.y = y;
    vertex.r = color.r;
    vertex.g = color.g;
    vertex.b = color.b;
    vertex.a = color.a * alpha;
    return vertex;
}

void push_triangle(TriangleList& out, const Vertex& a, const Vertex& b, const Vertex& c) {
    out.vertices.push_back(a);
    out.vertices.push_back(b);
    out.vertices.push_back(c);
}

void push_quad(
    TriangleList& out, const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    push_triangle(out, a, b, c);
    push_triangle(out, a, c, d);
}

void stroke_one(TriangleList& out, const Segment& segment, StrokeStyle style) {
    const float dx = segment.x1 - segment.x0;
    const float dy = segment.y1 - segment.y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length >= kMinLength) || !(style.width > 0.f)) {
        return;
    }
    const float inv = 1.f / length;
    const float dir_x = dx * inv;
    const float dir_y = dy * inv;
    const float normal_x = -dir_y;
    const float normal_y = dir_x;
    const float half = style.width * 0.5f;
    if (style.feather < 0.f) {
        style.feather = 0.f;
    }

    const auto at = [&](float along_x, float along_y, float normal_scale, float alpha) {
        return make_vertex(along_x + normal_x * normal_scale,
                           along_y + normal_y * normal_scale,
                           segment.color,
                           alpha);
    };
    const Vertex left0 = at(segment.x0, segment.y0, half, 1.f);
    const Vertex right0 = at(segment.x0, segment.y0, -half, 1.f);
    const Vertex left1 = at(segment.x1, segment.y1, half, 1.f);
    const Vertex right1 = at(segment.x1, segment.y1, -half, 1.f);
    push_quad(out, left0, left1, right1, right0);

    if (style.feather <= 0.f) {
        return;
    }
    const float outer = half + style.feather;
    const Vertex outer_left0 = at(segment.x0, segment.y0, outer, 0.f);
    const Vertex outer_right0 = at(segment.x0, segment.y0, -outer, 0.f);
    const Vertex outer_left1 = at(segment.x1, segment.y1, outer, 0.f);
    const Vertex outer_right1 = at(segment.x1, segment.y1, -outer, 0.f);
    push_quad(out, outer_left0, outer_left1, left1, left0);
    push_quad(out, right0, right1, outer_right1, outer_right0);
}

} // namespace

void append_strokes(TriangleList& out,
                    std::span<const Segment> segments,
                    StrokeStyle style,
                    std::span<const FilledTriangle> fills) {
    struct Ordered {
        float depth = 0;
        bool fill = false;
        std::size_t index = 0;
    };
    std::vector<Ordered> order;
    order.reserve(segments.size() + fills.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
        order.push_back(Ordered{segments[i].depth, false, i});
    }
    for (std::size_t i = 0; i < fills.size(); ++i) {
        order.push_back(Ordered{fills[i].depth, true, i});
    }
    std::stable_sort(order.begin(), order.end(), [](const Ordered& a, const Ordered& b) {
        return a.depth > b.depth;
    });
    for (const Ordered& item : order) {
        if (!item.fill) {
            stroke_one(out, segments[item.index], style);
            continue;
        }
        const FilledTriangle& triangle = fills[item.index];
        push_triangle(out,
                      make_vertex(triangle.x0, triangle.y0, triangle.color, 1.f),
                      make_vertex(triangle.x1, triangle.y1, triangle.color, 1.f),
                      make_vertex(triangle.x2, triangle.y2, triangle.color, 1.f));
    }
}

bool triangle_degenerate(const Vertex& a, const Vertex& b, const Vertex& c) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
        !std::isfinite(c.x) || !std::isfinite(c.y)) {
        return true;
    }
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    return !(std::fabs(cross) > kDegenerateCross);
}

bool triangle_outside(
    const Vertex& a, const Vertex& b, const Vertex& c, float width, float height) {
    if (!(width > 0.f) || !(height > 0.f)) {
        return false;
    }
    const float min_x = std::min(a.x, std::min(b.x, c.x));
    const float max_x = std::max(a.x, std::max(b.x, c.x));
    const float min_y = std::min(a.y, std::min(b.y, c.y));
    const float max_y = std::max(a.y, std::max(b.y, c.y));
    return max_x < 0.f || min_x > width || max_y < 0.f || min_y > height;
}

namespace {

enum class ClipEdge { Left, Right, Top, Bottom };

bool inside_edge(const Vertex& vertex, ClipEdge edge, float width, float height) {
    switch (edge) {
    case ClipEdge::Left:
        return vertex.x >= 0.f;
    case ClipEdge::Right:
        return vertex.x <= width;
    case ClipEdge::Top:
        return vertex.y >= 0.f;
    case ClipEdge::Bottom:
        return vertex.y <= height;
    }
    return false;
}

float edge_span(const Vertex& from, const Vertex& to, ClipEdge edge) {
    switch (edge) {
    case ClipEdge::Left:
    case ClipEdge::Right:
        return to.x - from.x;
    case ClipEdge::Top:
    case ClipEdge::Bottom:
        return to.y - from.y;
    }
    return 0.f;
}

float edge_distance(const Vertex& from, ClipEdge edge, float width, float height) {
    switch (edge) {
    case ClipEdge::Left:
        return 0.f - from.x;
    case ClipEdge::Right:
        return width - from.x;
    case ClipEdge::Top:
        return 0.f - from.y;
    case ClipEdge::Bottom:
        return height - from.y;
    }
    return 0.f;
}

Vertex lerp_vertex(const Vertex& from, const Vertex& to, float t) {
    Vertex vertex;
    vertex.x = from.x + (to.x - from.x) * t;
    vertex.y = from.y + (to.y - from.y) * t;
    vertex.r = from.r + (to.r - from.r) * t;
    vertex.g = from.g + (to.g - from.g) * t;
    vertex.b = from.b + (to.b - from.b) * t;
    vertex.a = from.a + (to.a - from.a) * t;
    return vertex;
}

// A triangle clipped to four edges has at most seven vertices.
constexpr int kClipVerts = 8;

struct ClipPoly {
    Vertex v[kClipVerts]{};
    int n = 0;
};

void clip_edge(ClipPoly& poly, ClipEdge edge, float width, float height) {
    if (poly.n <= 0) {
        return;
    }
    Vertex next[kClipVerts];
    int count = 0;
    int previous = poly.n - 1;
    bool previous_in = inside_edge(poly.v[previous], edge, width, height);
    for (int i = 0; i < poly.n; ++i) {
        const bool inside = inside_edge(poly.v[i], edge, width, height);
        if (inside != previous_in) {
            const float span = edge_span(poly.v[previous], poly.v[i], edge);
            if (std::fabs(span) > 1e-8f && count < kClipVerts) {
                const float t = std::clamp(
                    edge_distance(poly.v[previous], edge, width, height) / span, 0.f, 1.f);
                next[count++] = lerp_vertex(poly.v[previous], poly.v[i], t);
            }
        }
        if (inside && count < kClipVerts) {
            next[count++] = poly.v[i];
        }
        previous = i;
        previous_in = inside;
    }
    poly.n = count;
    for (int i = 0; i < count; ++i) {
        poly.v[i] = next[i];
    }
}

void clamp_view(Vertex& vertex, float width, float height) {
    vertex.x = std::clamp(vertex.x, 0.f, width);
    vertex.y = std::clamp(vertex.y, 0.f, height);
}

void append_clipped(TriangleList& out,
                    const Vertex& a,
                    const Vertex& b,
                    const Vertex& c,
                    float width,
                    float height) {
    ClipPoly polygon;
    polygon.n = 3;
    polygon.v[0] = a;
    polygon.v[1] = b;
    polygon.v[2] = c;
    clip_edge(polygon, ClipEdge::Left, width, height);
    clip_edge(polygon, ClipEdge::Right, width, height);
    clip_edge(polygon, ClipEdge::Top, width, height);
    clip_edge(polygon, ClipEdge::Bottom, width, height);
    if (polygon.n < 3) {
        return;
    }
    for (int i = 0; i < polygon.n; ++i) {
        clamp_view(polygon.v[i], width, height);
    }
    for (int i = 1; i + 1 < polygon.n; ++i) {
        const Vertex& p0 = polygon.v[0];
        const Vertex& p1 = polygon.v[i];
        const Vertex& p2 = polygon.v[i + 1];
        if (triangle_degenerate(p0, p1, p2)) {
            continue;
        }
        out.vertices.push_back(p0);
        out.vertices.push_back(p1);
        out.vertices.push_back(p2);
    }
}

} // namespace

TriangleList
drop_degenerate_triangles(const TriangleList& source, float view_width, float view_height) {
    TriangleList kept;
    const std::size_t count = source.vertices.size() - (source.vertices.size() % 3);
    kept.vertices.reserve(count);
    const bool clip = view_width > 0.f && view_height > 0.f;
    for (std::size_t i = 0; i + 2 < count; i += 3) {
        const Vertex& a = source.vertices[i];
        const Vertex& b = source.vertices[i + 1];
        const Vertex& c = source.vertices[i + 2];
        if (triangle_degenerate(a, b, c) || triangle_outside(a, b, c, view_width, view_height)) {
            continue;
        }
        if (!clip) {
            kept.vertices.push_back(a);
            kept.vertices.push_back(b);
            kept.vertices.push_back(c);
            continue;
        }
        append_clipped(kept, a, b, c, view_width, view_height);
    }
    return kept;
}

} // namespace oscilline
