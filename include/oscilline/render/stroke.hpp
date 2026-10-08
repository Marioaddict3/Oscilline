// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Expands screen-space lines into anti-aliased triangles.

#pragma once

#include <span>
#include <vector>

namespace oscilline {

struct Rgb {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;
};

struct Vertex {
    float x = 0;
    float y = 0;
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;
};

// Triangle list. Three vertices per triangle, no index buffer.
struct TriangleList {
    std::vector<Vertex> vertices;
};

struct Segment {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    // Larger values are farther from the camera and are stroked first.
    float depth = 0;
    Rgb color{};
};

struct StrokeStyle {
    // Core width in logical pixels, plus a transparent feather on each side.
    float width = 2.f;
    float feather = 1.f;
};

// One flat triangle, drawn in the same far-to-near pass as the strokes.
// Packet color, no feather. A zero-area triangle is left for
// drop_degenerate_triangles.
struct FilledTriangle {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float depth = 0;
    Rgb color{};
};

// Course ribbon, obstacles, figure, and damage strokes. A 1 px line in the
// 512×286 PAL frame, shown 4:3, is about 1.25 logical px wide (vertical) and
// 1.68 tall (horizontal). The previous 2 px core plus 1 px feather laid about
// twice that much ink. High confidence.
inline constexpr float kCourseStrokeWidth = 1.f;
inline constexpr float kCourseStrokeFeather = 0.5f;

// Expands screen-space segments into anti-aliased quads and emits filled
// triangles in the same list. A zero-length segment is skipped. Segments and
// fills are sorted far-to-near together before expansion.
void append_strokes(TriangleList& out,
                    std::span<const Segment> segments,
                    StrokeStyle style = {},
                    std::span<const FilledTriangle> fills = {});

// Twice the triangle area, in logical pixels. Anything this flat is dropped
// before it reaches the renderer.
inline constexpr float kDegenerateCross = 0.001f;

// True when a coordinate is non-finite or the triangle has no area.
[[nodiscard]] bool triangle_degenerate(const Vertex& a, const Vertex& b, const Vertex& c);

// Drops zero-area and non-finite triangles. When both view sizes are positive,
// a triangle that misses the view is dropped and one that crosses the edge is
// clipped to [0, view_width] x [0, view_height] before it reaches the renderer.
// A trailing partial triangle is dropped too.
[[nodiscard]] TriangleList drop_degenerate_triangles(const TriangleList& source,
                                                     float view_width = 0.f,
                                                     float view_height = 0.f);

} // namespace oscilline
