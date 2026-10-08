// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Draws the ribbon, obstacles, figure, HUD, and end screens.

#include "course_draw.hpp"

#include "figure.hpp"
#include "oscilline/asset/character.hpp"
#include "oscilline/asset/meter.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/figure.hpp"
#include "oscilline/course/jitter.hpp"
#include "oscilline/course/score.hpp"
#include "oscilline/course/shapes.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"
#include "score_tracker.hpp"
#include "text_out.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

constexpr float kRibbonY = 240.f;
constexpr float kPi = 3.14159265f;
// Logical px the disc-camera ribbon runs past each screen edge.
constexpr float kDiscRibbonExtendPx = 6000.f;

void translate_segments(std::vector<Segment>& segments, float dx, float dy) {
    if (dx == 0.f && dy == 0.f) {
        return;
    }
    for (Segment& segment : segments) {
        segment.x0 += dx;
        segment.y0 += dy;
        segment.x1 += dx;
        segment.y1 += dy;
    }
}

void translate_fills(std::vector<FilledTriangle>& fills, float dx, float dy) {
    if (dx == 0.f && dy == 0.f) {
        return;
    }
    for (FilledTriangle& fill : fills) {
        fill.x0 += dx;
        fill.y0 += dy;
        fill.x1 += dx;
        fill.y1 += dy;
        fill.x2 += dx;
        fill.y2 += dy;
    }
}

void add(std::vector<Segment>& out, float x0, float y0, float x1, float y1, float depth) {
    Segment segment;
    segment.x0 = x0;
    segment.y0 = y0;
    segment.x1 = x1;
    segment.y1 = y1;
    segment.depth = depth;
    out.push_back(segment);
}

void damage_strokes(std::vector<Segment>& out, float x, float y, int damage) {
    constexpr float kDepth = 0.f;
    const int marks = std::max(damage, 0);
    for (int i = 0; i < marks; ++i) {
        const float angle = 0.4f + static_cast<float>(i) * 0.62f;
        const float reach = 16.f + static_cast<float>(i % 4) * 5.f;
        const float jx = std::cos(angle) * reach;
        const float jy = std::sin(angle) * reach * 0.55f - 28.f;
        const float kink = (i % 2 == 0) ? 5.f : -5.f;
        add(out, x + jx, y + jy, x + jx * 0.35f + kink, y + jy * 0.45f, kDepth);
    }
}

void scribble_burst(
    std::vector<Segment>& out, float x, float y, std::int64_t time_ms, std::int64_t until_ms) {
    if (until_ms <= 0 || time_ms >= until_ms) {
        return;
    }
    const float life = static_cast<float>(until_ms - time_ms) / static_cast<float>(kFormBurstMs);
    constexpr float kDepth = 0.f;
    for (int i = 0; i < kFormBurstStrokes; ++i) {
        const float angle =
            static_cast<float>(i) * (kPi * 2.f / static_cast<float>(kFormBurstStrokes)) +
            0.2f * static_cast<float>(i % 3);
        const float inner = 8.f + static_cast<float>(i % 5) * 3.f;
        const float outer = inner + (10.f + static_cast<float>(i % 4) * 4.f) * life;
        add(out,
            x + std::cos(angle) * inner,
            y - 24.f + std::sin(angle) * inner,
            x + std::cos(angle) * outer,
            y - 24.f + std::sin(angle) * outer,
            kDepth);
    }
}

// Bottom arc. The yellow portion grows from the right end as `fill` goes 0 to 1.
// These proportions follow the PAL gameplay view: a wide, shallow curve. Both
// ends run 12 px below the bottom edge, so the screen clips them and the line
// ends never show. The crest stays at y 448.
void append_arc(std::vector<Segment>& out, float t0, float t1, bool filled_part) {
    const float offset = (static_cast<float>(logical_width() - kLogicalWidth)) * 0.5f;
    const float left = 104.f + offset;
    const float right = 536.f + offset;
    constexpr float kEndY = static_cast<float>(kLogicalHeight) + 12.f;
    constexpr float kCrestY = 448.f;
    // A quadratic Bezier's midpoint is halfway between its ends and its control.
    constexpr float kControlY = 2.f * kCrestY - kEndY;
    const auto point = [=](float t) {
        const float inverse = 1.f - t;
        const float middle = (left + right) * 0.5f;
        const float x = inverse * inverse * left + 2.f * inverse * t * middle + t * t * right;
        const float y = inverse * inverse * kEndY + 2.f * inverse * t * kControlY + t * t * kEndY;
        return std::pair{x, y};
    };
    const auto [x0, y0] = point(t0);
    const auto [x1, y1] = point(t1);
    Segment segment;
    segment.x0 = x0;
    segment.y0 = y0;
    segment.x1 = x1;
    segment.y1 = y1;
    segment.depth = 2.5f;
    segment.color = filled_part ? Rgb{0.95f, 0.82f, 0.2f, 1.f} : Rgb{0.2f, 0.8f, 0.35f, 1.f};
    out.push_back(segment);
}

// The color change is the fill fraction itself, not the nearest of the 32 steps.
void progress_arc(std::vector<Segment>& out, float fill) {
    if (!(fill > 0.f)) {
        fill = 0.f;
    }
    if (fill > 1.f) {
        fill = 1.f;
    }
    constexpr int kSteps = 32;
    const float boundary = 1.f - fill;
    for (int i = 0; i < kSteps; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(kSteps);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSteps);
        if (t1 <= boundary) {
            append_arc(out, t0, t1, false);
        } else if (t0 >= boundary) {
            append_arc(out, t0, t1, true);
        } else {
            append_arc(out, t0, boundary, false);
            append_arc(out, boundary, t1, true);
        }
    }
}

float ribbon_y_on_spine(const std::vector<Segment>& spine, float x, float fallback) {
    for (const Segment& segment : spine) {
        const float span = segment.x1 - segment.x0;
        if (!(span > 0.f) || x < segment.x0 || x > segment.x1) {
            continue;
        }
        const float t = (x - segment.x0) / span;
        return segment.y0 + (segment.y1 - segment.y0) * t;
    }
    return fallback;
}

// Screen-edge breaks plus each obstacle attachment. `attachment_x` holds
// left/right pairs, and the piece between an obstacle's own pair is left out
// because its outline carries the line there. Scroll ticks stay on the spine.
// The hit mark follows the spine and is not itself a break.
void ribbon(std::vector<Segment>& out,
            std::int64_t scroll_ms,
            std::int64_t jitter_ms,
            float y,
            float hit_x,
            std::span<const float> attachment_x,
            float amplitude_ps,
            bool guides,
            float extend = 0.f) {
    constexpr float kDepth = 2.f;
    constexpr float kTickLeft = 36.f;
    const float tick_right = static_cast<float>(logical_width()) - 20.f;
    // Under the disc camera the ribbon is a world line, so it runs on past the
    // screen edges and stays across the frame while the eye turns. Obstacles
    // past an edge break it like any other, and the screen edges stay breaks.
    const float width = static_cast<float>(logical_width());
    std::vector<float> breaks(attachment_x.begin(), attachment_x.end());
    if (extend > 0.f) {
        breaks.push_back(0.f);
        breaks.push_back(width);
    }
    std::vector<Segment> spine =
        ribbon_jitter_spine(-extend, width + extend, y, breaks, jitter_ms, amplitude_ps, kDepth);
    if (extend > 0.f && !spine.empty()) {
        // The far ends are off screen. Keep the outermost pieces level.
        spine.front().y0 = spine.front().y1;
        spine.back().y1 = spine.back().y0;
    }
    for (const Segment& piece : spine) {
        const float mid = (piece.x0 + piece.x1) * 0.5f;
        bool covered = false;
        for (std::size_t i = 0; i + 1 < attachment_x.size(); i += 2) {
            covered = covered || (mid > attachment_x[i] && mid < attachment_x[i + 1]);
        }
        if (!covered) {
            out.push_back(piece);
        }
    }
    if (!guides) {
        return;
    }
    const float scroll = std::fmod(static_cast<float>(scroll_ms) * 0.08f, 24.f);
    for (int tick = 0;; ++tick) {
        const float x = 40.f - scroll + static_cast<float>(tick) * 24.f;
        if (x >= tick_right) {
            break;
        }
        if (x > kTickLeft) {
            const float tick_y = ribbon_y_on_spine(spine, x, y);
            add(out, x, tick_y - 6.f, x, tick_y + 6.f, kDepth);
        }
    }
    const float mark_y = ribbon_y_on_spine(spine, hit_x, y);
    add(out, hit_x, mark_y - 36.f, hit_x, mark_y + 36.f, kDepth);
}

void apply_tilt(std::vector<Segment>& segments, float tilt_deg) {
    if (!(std::fabs(tilt_deg) > 0.05f)) {
        return;
    }
    const float rad = tilt_deg * kPi / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float cx = static_cast<float>(logical_width()) * 0.5f;
    const float cy = static_cast<float>(kLogicalHeight) * 0.5f;
    const auto rotate = [&](float& x, float& y) {
        const float dx = x - cx;
        const float dy = y - cy;
        x = cx + dx * c - dy * s;
        y = cy + dx * s + dy * c;
    };
    for (Segment& segment : segments) {
        rotate(segment.x0, segment.y0);
        rotate(segment.x1, segment.y1);
    }
}

void apply_tilt_fills(std::vector<FilledTriangle>& fills, float tilt_deg) {
    if (!(std::fabs(tilt_deg) > 0.05f)) {
        return;
    }
    const float rad = tilt_deg * kPi / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float cx = static_cast<float>(logical_width()) * 0.5f;
    const float cy = static_cast<float>(kLogicalHeight) * 0.5f;
    const auto rotate = [&](float& x, float& y) {
        const float dx = x - cx;
        const float dy = y - cy;
        x = cx + dx * c - dy * s;
        y = cy + dx * s + dy * c;
    };
    for (FilledTriangle& fill : fills) {
        rotate(fill.x0, fill.y0);
        rotate(fill.x1, fill.y1);
        rotate(fill.x2, fill.y2);
    }
}

// Scale and roll around the figure. The built-in path does not call this.
DiscView disc_view(const AncSample& sample) {
    DiscView view;
    view.eye_x = sample.eye_x;
    view.eye_y = sample.eye_y;
    view.eye_z = sample.eye_z;
    view.target_x = sample.target_x;
    view.target_y = sample.target_y;
    view.target_z = sample.target_z;
    view.projection_h = anc_projection_h(sample.fov);
    return view;
}

// One world for the course and the figure. The 2D layout is the reference
// camera's picture of the ribbon plane (world x = 0, feet on y = 0, the ribbon
// along z). Each layout point is lifted onto that plane by the reference
// camera's local scale at the origin and projected with the current eye, the
// same projection paint_disc_figure uses, so her feet stay on the ribbon.
// There is no extra screen shift: the ANC eye, target and projection distance
// frame the ribbon and Rabbit as the original does (#37 framing data).
struct DiscWorld {
    bool ok = false;
    DiscView now;
    float origin_x = 0.f;
    float origin_y = 0.f;
    // Screen offset from the origin to world (z, y).
    float inverse[4] = {};
};

DiscWorld make_disc_world(const AncSample& reference, const AncSample& now, float w, float h) {
    DiscWorld out;
    const DiscView ref = disc_view(reference);
    DiscProjected origin;
    DiscProjected z_plus;
    DiscProjected z_minus;
    DiscProjected y_plus;
    DiscProjected y_minus;
    if (!project_disc_world(ref, {0.f, 0.f, 0.f}, w, h, origin) ||
        !project_disc_world(ref, {0.f, 0.f, 1.f}, w, h, z_plus) ||
        !project_disc_world(ref, {0.f, 0.f, -1.f}, w, h, z_minus) ||
        !project_disc_world(ref, {0.f, 1.f, 0.f}, w, h, y_plus) ||
        !project_disc_world(ref, {0.f, -1.f, 0.f}, w, h, y_minus)) {
        return out;
    }
    const float zx = 0.5f * (z_plus.x - z_minus.x);
    const float zy = 0.5f * (z_plus.y - z_minus.y);
    const float yx = 0.5f * (y_plus.x - y_minus.x);
    const float yy = 0.5f * (y_plus.y - y_minus.y);
    const float det = zx * yy - yx * zy;
    if (!std::isfinite(det) || !(std::fabs(det) > 1.e-6f)) {
        return out;
    }
    out.inverse[0] = yy / det;
    out.inverse[1] = -yx / det;
    out.inverse[2] = -zy / det;
    out.inverse[3] = zx / det;
    out.origin_x = origin.x;
    out.origin_y = origin.y;
    out.now = disc_view(now);
    out.ok = std::isfinite(out.origin_x) && std::isfinite(out.origin_y);
    return out;
}

ModelVertex lift_to_world(const DiscWorld& map, float x, float y) {
    const float dx = x - map.origin_x;
    const float dy = y - map.origin_y;
    ModelVertex world;
    world.z = map.inverse[0] * dx + map.inverse[1] * dy;
    world.y = map.inverse[2] * dx + map.inverse[3] * dy;
    return world;
}

void project_world_segments(std::vector<Segment>& segments, const DiscWorld& map) {
    constexpr float kNear = 1.f;
    std::vector<Segment> out;
    out.reserve(segments.size());
    for (const Segment& segment : segments) {
        ModelVertex a = lift_to_world(map, segment.x0, segment.y0);
        ModelVertex b = lift_to_world(map, segment.x1, segment.y1);
        float depth_a = 0.f;
        float depth_b = 0.f;
        if (!disc_view_depth(map.now, a, depth_a) || !disc_view_depth(map.now, b, depth_b) ||
            (depth_a < kNear && depth_b < kNear)) {
            continue;
        }
        const auto clip = [&](ModelVertex& behind,
                              float depth_behind,
                              const ModelVertex& front,
                              float depth_front) {
            const float t = (kNear - depth_behind) / (depth_front - depth_behind);
            behind.y += (front.y - behind.y) * t;
            behind.z += (front.z - behind.z) * t;
        };
        if (depth_a < kNear) {
            clip(a, depth_a, b, depth_b);
        } else if (depth_b < kNear) {
            clip(b, depth_b, a, depth_a);
        }
        const float w = static_cast<float>(logical_width());
        const float h = static_cast<float>(kLogicalHeight);
        DiscProjected pa;
        DiscProjected pb;
        if (!project_disc_world(map.now, a, w, h, pa) ||
            !project_disc_world(map.now, b, w, h, pb)) {
            continue;
        }
        Segment moved = segment;
        moved.x0 = pa.x;
        moved.y0 = pa.y;
        moved.x1 = pb.x;
        moved.y1 = pb.y;
        out.push_back(moved);
    }
    segments.swap(out);
}

void project_world_fills(std::vector<FilledTriangle>& fills, const DiscWorld& map) {
    const float w = static_cast<float>(logical_width());
    const float h = static_cast<float>(kLogicalHeight);
    std::vector<FilledTriangle> out;
    out.reserve(fills.size());
    for (const FilledTriangle& fill : fills) {
        DiscProjected a;
        DiscProjected b;
        DiscProjected c;
        if (!project_disc_world(map.now, lift_to_world(map, fill.x0, fill.y0), w, h, a) ||
            !project_disc_world(map.now, lift_to_world(map, fill.x1, fill.y1), w, h, b) ||
            !project_disc_world(map.now, lift_to_world(map, fill.x2, fill.y2), w, h, c)) {
            continue;
        }
        FilledTriangle moved = fill;
        moved.x0 = a.x;
        moved.y0 = a.y;
        moved.x1 = b.x;
        moved.y1 = b.y;
        moved.x2 = c.x;
        moved.y2 = c.y;
        out.push_back(moved);
    }
    fills.swap(out);
}

void apply_disc_frame(
    std::vector<Segment>& segments, float pivot_x, float pivot_y, float tilt_deg, float zoom) {
    const bool scale = std::isfinite(zoom) && zoom > 0.f && std::fabs(zoom - 1.f) > 1.e-4f;
    const bool rotate = std::isfinite(tilt_deg) && std::fabs(tilt_deg) > 0.05f;
    if (!scale && !rotate) {
        return;
    }
    const float rad = tilt_deg * kPi / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const auto map = [&](float& x, float& y) {
        float dx = x - pivot_x;
        float dy = y - pivot_y;
        if (scale) {
            dx *= zoom;
            dy *= zoom;
        }
        if (rotate) {
            const float rx = dx * c - dy * s;
            const float ry = dx * s + dy * c;
            dx = rx;
            dy = ry;
        }
        x = pivot_x + dx;
        y = pivot_y + dy;
    };
    for (Segment& segment : segments) {
        map(segment.x0, segment.y0);
        map(segment.x1, segment.y1);
    }
}

void apply_disc_fills(
    std::vector<FilledTriangle>& fills, float pivot_x, float pivot_y, float tilt_deg, float zoom) {
    const bool scale = std::isfinite(zoom) && zoom > 0.f && std::fabs(zoom - 1.f) > 1.e-4f;
    const bool rotate = std::isfinite(tilt_deg) && std::fabs(tilt_deg) > 0.05f;
    if (!scale && !rotate) {
        return;
    }
    const float rad = tilt_deg * kPi / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const auto map = [&](float& x, float& y) {
        float dx = x - pivot_x;
        float dy = y - pivot_y;
        if (scale) {
            dx *= zoom;
            dy *= zoom;
        }
        if (rotate) {
            const float rx = dx * c - dy * s;
            const float ry = dx * s + dy * c;
            dx = rx;
            dy = ry;
        }
        x = pivot_x + dx;
        y = pivot_y + dy;
    };
    for (FilledTriangle& fill : fills) {
        map(fill.x0, fill.y0);
        map(fill.x1, fill.y1);
        map(fill.x2, fill.y2);
    }
}

void glyph(CourseFrame& frame,
           const TextPainter& painter,
           float x,
           float y,
           std::string value,
           TextStyle style = {}) {
    TextTarget target;
    target.glyphs = &frame.text;
    target.triangles = &frame.triangles;
    painter.line(target, x, y, std::move(value), style);
}

void menu_choice(CourseFrame& frame,
                 const TextPainter& painter,
                 float x,
                 float y,
                 bool selected,
                 std::string label) {
    glyph(frame, painter, x, y, "  " + std::move(label));
    if (selected) {
        paint_menu_selector(frame.triangles, x, y);
    }
}

Rgb judgment_color(Judgment judgment) {
    switch (judgment) {
    case Judgment::Perfect:
        return {0.95f, 0.82f, 0.2f, 1.f};
    case Judgment::Good:
        return {0.55f, 0.85f, 1.f, 1.f};
    case Judgment::Miss:
        return {1.f, 0.45f, 0.4f, 1.f};
    case Judgment::None:
        return {};
    }
    return {};
}

// Higher fill is further from super. The meter frames for four forms were
// not measured, so the steps are even. Medium.
float evolution_fill(Form form) {
    switch (form) {
    case Form::Super:
        return 0.f;
    case Form::Rabbit:
        return 1.f / 3.f;
    case Form::Frog:
        return 2.f / 3.f;
    case Form::Worm:
    case Form::Out:
        return 1.f;
    }
    return 0.f;
}

void streak_ring(std::vector<Segment>& out,
                 float figure_x,
                 float ribbon_y,
                 float screen_h,
                 const PlayState& play) {
    const int count = streak_ring_dots(play.form, play.clear_run);
    if (count <= 0) {
        return;
    }
    const float half_x = 0.5f * kStreakDotPs * kPsToLogicalX;
    const float half_y = 0.5f * kStreakDotPs * kPsToLogicalY;
    const Rgb pink{static_cast<float>(kStreakDotR) / 255.f,
                   static_cast<float>(kStreakDotG) / 255.f,
                   static_cast<float>(kStreakDotB) / 255.f,
                   1.f};
    for (int i = 0; i < count; ++i) {
        const StreakDot dot = streak_ring_dot(figure_x, ribbon_y, screen_h, i);
        const auto side = [&](float x0, float y0, float x1, float y1) {
            Segment segment;
            segment.x0 = x0;
            segment.y0 = y0;
            segment.x1 = x1;
            segment.y1 = y1;
            segment.depth = 0.f;
            segment.color = pink;
            out.push_back(segment);
        };
        side(dot.x - half_x, dot.y - half_y, dot.x + half_x, dot.y - half_y);
        side(dot.x + half_x, dot.y - half_y, dot.x + half_x, dot.y + half_y);
        side(dot.x + half_x, dot.y + half_y, dot.x - half_x, dot.y + half_y);
        side(dot.x - half_x, dot.y + half_y, dot.x - half_x, dot.y - half_y);
    }
}

// Seventeen radial dashes. The center is the figure's screen position at t = 0
// (the ring center, 0.104 H above the ribbon) and stays there. No fade: the
// dashes are absent once the timer ends. Color is low confidence. The stroke
// uses the same default width as the ribbon.
void append_super_burst(std::vector<Segment>& out,
                        const PlayState& play,
                        std::int32_t duration_ms,
                        float screen_w,
                        float screen_h,
                        std::int64_t time_ms,
                        const DiscCameraPaths& cameras,
                        bool disc_camera) {
    const SuperTransform& effect = play.super_transform;
    if (!effect.active_at(time_ms)) {
        return;
    }
    const CameraPose at_start = gameplay_camera(
        effect.start_ms, duration_ms, screen_w, screen_h, true, cameras, disc_camera);
    const float anchor_y = at_start.disc ? at_start.figure_y : kRibbonY;
    // Segmented jitter left the ribbon on its anchor, so the center does not follow the onset.
    const float ribbon_at_start = anchor_y;
    float center_x = at_start.figure_x;
    float center_y = ribbon_at_start - kStreakCenterAboveRibbon * screen_h;
    if (at_start.disc && std::isfinite(at_start.zoom) && at_start.zoom > 0.f) {
        center_x = at_start.figure_x + (center_x - at_start.figure_x) * at_start.zoom;
        center_y = ribbon_at_start + (center_y - ribbon_at_start) * at_start.zoom;
    }
    const float u = super_burst_u(effect.start_ms, time_ms);
    const Rgb color{static_cast<float>(kSuperDashR) / 255.f,
                    static_cast<float>(kSuperDashG) / 255.f,
                    static_cast<float>(kSuperDashB) / 255.f,
                    1.f};
    for (int slot = 0; slot < kStreakSlotCount; ++slot) {
        const SuperDash dash = super_burst_dash(center_x, center_y, screen_h, slot, u);
        Segment segment;
        segment.x0 = dash.x0;
        segment.y0 = dash.y0;
        segment.x1 = dash.x1;
        segment.y1 = dash.y1;
        segment.depth = 0.f;
        segment.color = color;
        out.push_back(segment);
    }
}

ScreenRect progress_box() {
    const float kCx = static_cast<float>(logical_width()) * 0.5f;
    constexpr float kCy = 560.f;
    constexpr float kRadius = 130.f;
    constexpr float kRight = kPi * 0.15f;
    constexpr float kLeft = kPi * 0.85f;
    ScreenRect rect;
    rect.left = kCx + std::cos(kLeft) * kRadius;
    rect.right = kCx + std::cos(kRight) * kRadius;
    rect.top = kCy - kRadius;
    const float end_y = kCy - std::sin(kRight) * kRadius;
    rect.bottom = std::min(static_cast<float>(kLogicalHeight) - 1.f, end_y);
    return rect;
}

bool draw_model_frame(
    std::vector<Segment>& out, AssetRegistry& assets, Slot slot, int frame, ScreenRect rect) {
    if (assets.origin(slot) != AssetOrigin::Disc) {
        return false;
    }
    const TmdModel* model = assets.model(slot);
    if (model == nullptr) {
        return false;
    }
    std::vector<Pose> poses;
    std::span<const Pose> pose_span;
    if (const auto* animations = assets.animations(slot);
        animations != nullptr && !animations->empty()) {
        const int count = static_cast<int>(animations->front().frames.size());
        if (frame < 0) {
            frame = 0;
        }
        if (count > 0 && frame >= count) {
            frame = count - 1;
        }
        poses = poses_for_frame(animations->front(), model->objects.size(), frame, 0.f, false);
        pose_span = poses;
    }
    WireframeOptions options;
    options.packet_color = true;
    const std::vector<ModelSegment> lines = tmd_wireframe(*model, pose_span, options);
    const std::vector<Segment> fitted = fit_model_xy(lines, rect);
    if (fitted.empty()) {
        return false;
    }
    out.insert(out.end(), fitted.begin(), fitted.end());
    return true;
}

bool draw_digits(std::vector<Segment>& out, AssetRegistry& assets, int value, float x, float y) {
    if (assets.origin(Slot::ResultsDigits) != AssetOrigin::Disc) {
        return false;
    }
    const TmdModel* model = assets.model(Slot::ResultsDigits);
    if (model == nullptr || model->objects.size() < 10) {
        return false;
    }
    if (value < 0) {
        value = 0;
    }
    const std::string digits = std::to_string(value);
    float cursor = x;
    constexpr float kDigit = 14.f;
    constexpr float kHeight = 12.f;
    bool any = false;
    for (char ch : digits) {
        const auto object = digit_object(ch - '0', static_cast<int>(model->objects.size()));
        if (!object) {
            return false;
        }
        std::vector<Pose> poses(model->objects.size());
        poses[static_cast<std::size_t>(*object)].visible = true;
        WireframeOptions options;
        options.packet_color = true;
        const std::vector<ModelSegment> lines = tmd_wireframe(*model, poses, options);
        ScreenRect rect;
        rect.left = cursor;
        rect.top = y;
        rect.right = cursor + kDigit - 2.f;
        rect.bottom = y + kHeight;
        const std::vector<Segment> fitted = fit_model_xy(lines, rect);
        out.insert(out.end(), fitted.begin(), fitted.end());
        any = any || !fitted.empty();
        cursor += kDigit;
    }
    return any;
}

std::string form_label(Form form) {
    std::string name(form_name(form));
    for (char& letter : name) {
        letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    return name;
}

} // namespace

std::vector<Segment> progress_arc_segments(float fill) {
    std::vector<Segment> segments;
    progress_arc(segments, fill);
    return segments;
}

void PlaceholderFigure::paint(std::vector<Segment>& out, Form form, float x, float y) const {
    placeholder_figure(out, form, x, y);
}

CourseFrame draw_course(const CourseTimeline& course,
                        const PlayState& play,
                        std::int64_t time_ms,
                        const CourseView& view) {
    const int menu = view.menu;
    const bool show_end = view.show_end;
    const int round_number = view.round_number;
    const int prior_score = view.prior.score;
    const int prior_perfects = view.prior.perfects;
    const int prior_goods = view.prior.goods;
    const int prior_misses = view.prior.misses;
    const std::string_view heading = view.heading;
    const TextPainter* text = view.text;
    AssetRegistry* assets = view.assets;
    const DiscFigurePose* figure = view.figure;
    const bool disc_camera = view.disc_camera;
    const bool ribbon_guides = view.ribbon_guides;
    const CourseHud& hud = view.hud;
    DebugTextPainter debug_text;
    const TextPainter& painter = text != nullptr ? *text : debug_text;
    painter.set_time_ms(time_ms);
    const float screen_w = static_cast<float>(logical_width());
    const float center_offset = (screen_w - static_cast<float>(kLogicalWidth)) * 0.5f;
    const float screen_h = static_cast<float>(kLogicalHeight);
    DiscCameraPaths cameras;
    cameras.intro = view.cameras.intro;
    cameras.play = view.cameras.play;
    cameras.s02 = view.cameras.s02;
    cameras.b01 = view.cameras.b01;
    cameras.tv_bb = view.cameras.tv_bb;
    cameras.tv_bs = view.cameras.tv_bs;
    cameras.course_index = course.track_index;
    cameras.sections = course.camera_sections;
    std::vector<CameraObstacle> camera_obstacles;
    camera_obstacles.reserve(course.events.size());
    for (const CourseEvent& event : course.events) {
        CameraObstacle obstacle;
        obstacle.hit_ms = event.hit_ms;
        const std::int32_t travel = stage_scroll_approach_ms(event);
        obstacle.approach_ms = travel > 0 ? travel : 0;
        obstacle.window_open_ms = obstacle_window(event).good_open;
        obstacle.has_window = true;
        camera_obstacles.push_back(obstacle);
    }
    cameras.obstacles = camera_obstacles;
    // Custom bronze uses course 1's road files. The stage-start pan plays on
    // every course. An empty obstacle list keeps the watched boundary spins.
    const bool watched_course_one =
        course.track_index == 0 && course.cdda_track == 2 && !course.camera_sections.empty();
    cameras.gate_transitions = !watched_course_one;
    cameras.audio_end_ms = course.audio_end_ms;
    // Layout stays on the camera. The Gold shift is a screen translation after it.
    cameras.gold = false;
    const CameraPose pose = gameplay_camera(
        time_ms, course.duration_ms, screen_w, screen_h, true, cameras, disc_camera);
    const float spawn_x = screen_w * kSpawnXFraction;
    const AncSample* figure_camera = nullptr;
    AncSample held_camera;
    DiscWorld disc_world;
    if (disc_camera) {
        const DiscCameraSample picked = disc_camera_sample(time_ms, true, cameras);
        if (picked.valid) {
            held_camera = picked.sample;
            figure_camera = &held_camera;
            // The layout is drawn as the play camera sees it (the last intro key without one).
            const AncFile* play_camera = view.cameras.play;
            const AncFile* intro_camera = view.cameras.intro;
            const bool have_play = play_camera != nullptr && !play_camera->keys.empty();
            const AncSample reference =
                have_play ? sample_anc_at(*play_camera, 0.f)
                          : sample_anc_at(*intro_camera,
                                          static_cast<float>(intro_camera->keys.size() - 1));
            disc_world = make_disc_world(reference, held_camera, screen_w, screen_h);
        }
    }
    const bool shared_world = pose.disc && disc_world.ok;
    const float figure_x = shared_world ? disc_world.origin_x : pose.figure_x;
    const float anchor_y =
        shared_world ? disc_world.origin_y : (pose.disc ? pose.figure_y : kRibbonY);
    // Segmented jitter replaces the onset translation.
    const float ribbon_y = anchor_y;
    const float loop_px =
        std::min(screen_w, static_cast<float>(kLogicalWidth)) * kLoopWidthFraction;
    const float amplitude_ps =
        ribbon_jitter_amplitude_ps(play.form, play.hits_since_form, play.last_hit_ms, time_ms);

    struct PlacedObstacle {
        std::uint8_t id = 0;
        float x = 0.f;
        float scale = 1.f;
        std::uint32_t salt = 0;
    };
    std::vector<PlacedObstacle> placed;
    std::vector<float> attachment_x;
    std::vector<Segment> window_guides;
    // Out ends the course. Leave the ribbon and skip the backlog. A cleared
    // course keeps already-spawned obstacles moving through the outro.
    if (play.form != Form::Out) {
        constexpr double kScrollTimeScale = 1.0 / kStageScrollSpeedScale;
        constexpr std::int64_t kAheadMs =
            static_cast<std::int64_t>(kMaxApproachMs * kScrollTimeScale) + 200;
        // A passed obstacle keeps moving left for a full approach, which is at
        // least one screen width past the figure. Off-screen ones are culled by x.
        constexpr std::int64_t kPastMs = kAheadMs;
        constexpr std::size_t kMaxDrawn = 96;
        // The visible stretch of the layout. The disc camera reprojects the
        // layout, and a zoomed-out or tilted disc frame shows past its edges,
        // so those keep obstacles as far out as the ribbon is drawn.
        float visible_left = 0.f;
        float visible_right = screen_w;
        if (shared_world) {
            visible_left = -kDiscRibbonExtendPx;
            visible_right = screen_w + kDiscRibbonExtendPx;
        } else if (pose.disc) {
            const float zoom = pose.zoom > 0.f ? pose.zoom : 1.f;
            const float margin = screen_w * 0.5f;
            visible_left = figure_x - figure_x / zoom - margin;
            visible_right = figure_x + (screen_w - figure_x) / zoom + margin;
        }
        const auto& events = course.events;
        const std::int64_t earliest = time_ms > kPastMs ? time_ms - kPastMs : 0;
        auto first = std::lower_bound(
            events.begin(), events.end(), earliest, [](const CourseEvent& event, std::int64_t hit) {
                return event.hit_ms < hit;
            });
        auto last = std::upper_bound(
            first,
            events.end(),
            time_ms + kAheadMs,
            [](std::int64_t hit, const CourseEvent& event) { return hit < event.hit_ms; });
        // Events run left to right. A cap drops the farthest-ahead ones, never
        // an obstacle still on its way off the left.
        for (auto it = first; it != last && placed.size() < kMaxDrawn; ++it) {
            const std::int64_t moving_time_ms = time_ms;
            const std::int32_t travel = stage_scroll_approach_ms(*it);
            if (travel <= 0) {
                continue;
            }
            if (obstacle_after_audio(it->hit_ms, travel, course.audio_end_ms)) {
                continue;
            }
            const float x =
                obstacle_screen_x(figure_x, spawn_x, travel, it->hit_ms, moving_time_ms);
            // Keep each obstacle at its designed size when approach windows overlap.
            constexpr float scale = 1.f;
            const float obstacle_width_px = obstacle_width(it->obstacle, loop_px * scale);
            const float obstacle_left = x + obstacle_leading_x(it->obstacle, loop_px * scale);
            if (obstacle_left + obstacle_width_px < visible_left || obstacle_left > visible_right) {
                continue;
            }
            const RibbonAttachment attachment =
                obstacle_ribbon_attachment(it->obstacle, loop_px * scale);
            if (attachment.valid) {
                attachment_x.push_back(x + attachment.left);
                attachment_x.push_back(x + attachment.right);
            }
            PlacedObstacle item;
            item.id = it->obstacle;
            item.x = x;
            item.scale = scale;
            item.salt = static_cast<std::uint32_t>(it->hit_ms) ^
                        (static_cast<std::uint32_t>(it->obstacle) << 16);
            placed.push_back(item);
            if (ribbon_guides) {
                const ObstacleWindow window = obstacle_window(*it);
                const Rgb perfect{0.95f, 0.82f, 0.2f, 1.f};
                const Rgb good{0.45f, 0.75f, 1.f, 1.f};
                const auto tick_at = [&](std::int64_t when, Rgb color) {
                    const std::int64_t guide_time_ms = time_ms;
                    const float wx = obstacle_screen_x(
                        figure_x, spawn_x, travel, static_cast<std::int32_t>(when), guide_time_ms);
                    Segment mark;
                    mark.x0 = wx;
                    mark.y0 = ribbon_y - 18.f;
                    mark.x1 = wx;
                    mark.y1 = ribbon_y + 18.f;
                    mark.depth = 1.5f;
                    mark.color = color;
                    window_guides.push_back(mark);
                };
                // Same intervals play_advance uses. Perfect ends at the biased front
                // edge; good continues kGoodIntoObstacleMs past it.
                tick_at(window.perfect_open, perfect);
                tick_at(window.perfect_close, perfect);
                tick_at(window.good_open, good);
                tick_at(window.good_close, good);
            }
        }
    }

    std::vector<Segment> world;
    ribbon(world,
           time_ms + kCourseStartDelayMs,
           time_ms,
           ribbon_y,
           figure_x,
           attachment_x,
           amplitude_ps,
           ribbon_guides,
           pose.disc ? kDiscRibbonExtendPx : 0.f);
    world.insert(world.end(), window_guides.begin(), window_guides.end());
    std::vector<Segment> figure_lines;
    std::vector<FilledTriangle> figure_fills;
    bool projected = false;
    const bool disc_figure =
        figure != nullptr &&
        paint_disc_figure(
            figure_lines, *figure, figure_x, ribbon_y, &figure_fills, figure_camera, &projected);
    if (!disc_figure) {
        PlaceholderFigure{}.paint(figure_lines, play.form, figure_x, ribbon_y);
        projected = false;
    }
    jitter_figure_vertices(figure_lines, time_ms, amplitude_ps, 0xF16u);
    std::vector<Segment> marks;
    damage_strokes(marks, figure_x, ribbon_y, play.damage);
    jitter_figure_vertices(marks, time_ms, amplitude_ps, 0xF17u);
    // The ring is not part of her lines, so damage jitter leaves it alone.
    streak_ring(marks, figure_x, ribbon_y, screen_h, play);
    scribble_burst(
        marks, figure_x, ribbon_y, std::max<std::int64_t>(time_ms, 0), play.burst_until_ms);
    for (const PlacedObstacle& item : placed) {
        append_jittered_obstacle(world,
                                 item.id,
                                 item.x,
                                 ribbon_y,
                                 loop_px,
                                 item.scale,
                                 time_ms,
                                 amplitude_ps,
                                 item.salt);
    }
    if (shared_world) {
        project_world_segments(world, disc_world);
        if (!projected) {
            project_world_segments(figure_lines, disc_world);
            project_world_fills(figure_fills, disc_world);
        }
        project_world_segments(marks, disc_world);
    } else if (pose.disc) {
        // The ribbon and the obstacles take the road angle. A flat figure stays
        // upright and scales with the camera. A projected figure already carries
        // the camera, so it is not tilted or zoomed again.
        apply_disc_frame(world, figure_x, ribbon_y, pose.tilt_deg, pose.zoom);
        if (!projected) {
            apply_disc_frame(figure_lines, figure_x, ribbon_y, 0.f, pose.zoom);
            apply_disc_fills(figure_fills, figure_x, ribbon_y, 0.f, pose.zoom);
        }
        apply_disc_frame(marks, figure_x, ribbon_y, 0.f, pose.zoom);
    } else {
        apply_tilt(world, pose.tilt_deg);
        apply_tilt(figure_lines, pose.tilt_deg);
        apply_tilt_fills(figure_fills, pose.tilt_deg);
        apply_tilt(marks, pose.tilt_deg);
    }

    if (disc_camera && course.gold_shift) {
        const GoldShiftPs shift = gold_shift_ps(time_ms);
        const float dx = shift.figure_dx * screen_w / kDiscBufferWidth;
        const float dy = shift.ribbon_dy * screen_h / kDiscBufferHeight;
        translate_segments(world, dx, dy);
        translate_segments(figure_lines, dx, dy);
        translate_fills(figure_fills, dx, dy);
        translate_segments(marks, dx, dy);
    }
    // Screen space, after the camera, so the burst stays where the figure was
    // at t = 0 while the ribbon and obstacles keep moving.
    DiscCameraPaths burst_cameras = cameras;
    burst_cameras.gold = disc_camera && course.gold_shift;
    append_super_burst(
        marks, play, course.duration_ms, screen_w, screen_h, time_ms, burst_cameras, disc_camera);
    std::vector<Segment> segments;
    std::vector<Segment> coupons;
    const int hud_score = play.score + std::max(prior_score, 0);
    if (!show_end && !hud.score_number) {
        const int score = std::min(kMaxCouponScore, hud_score);
        coupons = score_tracker_segments(score, score_tracker_phase(course, time_ms), assets);
    }
    const float fill = course_progress(time_ms, course.duration_ms);
    bool disc_progress = false;
    if (!show_end && assets != nullptr) {
        const int frames = [&] {
            const auto* animations = assets->animations(Slot::MeterProgress);
            if (animations == nullptr || animations->empty()) {
                return 1;
            }
            return std::max(1, static_cast<int>(animations->front().frames.size()));
        }();
        disc_progress = draw_model_frame(segments,
                                         *assets,
                                         Slot::MeterProgress,
                                         meter_frame_index(fill, frames),
                                         progress_box());
        if (assets->origin(Slot::MeterEvolution) == AssetOrigin::Disc) {
            const auto* animations = assets->animations(Slot::MeterEvolution);
            const int evo_frames =
                animations == nullptr || animations->empty()
                    ? 1
                    : std::max(1, static_cast<int>(animations->front().frames.size()));
            ScreenRect evo;
            evo.left = 24.f;
            evo.top = 64.f;
            evo.right = 96.f;
            evo.bottom = 136.f;
            draw_model_frame(segments,
                             *assets,
                             Slot::MeterEvolution,
                             meter_frame_index(evolution_fill(play.form), evo_frames),
                             evo);
        }
    }
    if (!show_end && !disc_progress) {
        progress_arc(segments, fill);
    }

    CourseFrame frame;
    StrokeStyle course_style;
    course_style.width = kCourseStrokeWidth;
    course_style.feather = kCourseStrokeFeather;
    if (!show_end) {
        append_strokes(frame.triangles, world, course_style);
        append_strokes(frame.triangles, figure_lines, course_style, figure_fills);
        append_strokes(frame.triangles, marks, course_style);
        append_strokes(frame.triangles, coupons, StrokeStyle{1.f, 0.5f});
    }
    append_strokes(frame.triangles, segments);
    const std::size_t stroked = segments.size();
    // Course and round labels sit bottom right, right-aligned, with the round above.
    const float hud_right = screen_w - 16.f;
    constexpr float kCourseLabelY = static_cast<float>(kLogicalHeight) - 28.f;
    const auto right_label = [&](float y, std::string value) {
        // Measure before the move; argument evaluation order is unspecified.
        const float x = hud_right - painter.measure_width(value);
        glyph(frame, painter, x, y, std::move(value));
    };
    if (!show_end) {
        right_label(kCourseLabelY,
                    heading.empty() ? "COURSE " + std::to_string(course.track_index + 1)
                                    : std::string(heading));
    }
    if (round_number > 0 && time_ms < -kCameraIntroMs && !show_end) {
        // The disc caption keeps its 240 x 52 aspect, scaled to clear the progress arc.
        ScreenRect caption;
        caption.right = hud_right;
        caption.left = hud_right - 160.f;
        caption.bottom = kCourseLabelY - 4.f;
        caption.top = caption.bottom - 160.f * 52.f / 240.f;
        const bool disc_caption =
            assets != nullptr &&
            draw_model_frame(segments, *assets, Slot::RoundCaption, round_number - 1, caption);
        if (!disc_caption) {
            right_label(kCourseLabelY - 18.f, "ROUND " + std::to_string(round_number));
        }
    }
    if (!show_end && hud.score_number) {
        // Centered where the coupon carousel would sit.
        const std::string value = std::to_string(hud_score);
        glyph(frame,
              painter,
              (screen_w - painter.measure_width(value)) * 0.5f,
              kCouponTrackY - 6.f,
              value);
    }
    if (!show_end && !play.paused) {
        if (!hud.control_hint.empty()) {
            glyph(frame, painter, 16.f, 16.f, std::string(hud.control_hint));
        }
        if (hud.timing_hints && play.last != Judgment::None && time_ms - play.judged_ms < 800) {
            TextStyle style;
            style.color = judgment_color(play.last);
            // Below the coupon carousel.
            glyph(frame,
                  painter,
                  280.f + center_offset,
                  104.f,
                  std::string(judgment_name(play.last)),
                  style);
        }
    }
    // End-screen copy sits on a separate, centered results screen.
    constexpr float kEndBandY = 112.f;
    constexpr float kEndStep = 18.f;
    const auto band = [](int row) { return kEndBandY + static_cast<float>(row) * kEndStep; };
    const auto score_lines = [&](int& row) {
        RoundScores scores;
        scores.latest = result_score(play);
        scores.paired = prior_score >= 0;
        scores.earlier = scores.paired ? prior_score : 0;
        const auto score_row = [&](const char* label, int value) {
            const float y = band(row++);
            const bool drew_digits =
                assets != nullptr &&
                draw_digits(segments, *assets, value, 360.f + center_offset, y);
            if (drew_digits) {
                glyph(frame, painter, 220.f + center_offset, y, label);
                return;
            }
            glyph(frame,
                  painter,
                  220.f + center_offset,
                  y,
                  std::string(label) + " " + std::to_string(value));
        };
        const int base = pair_total(scores);
        const int bonus = play.finished && play.form != Form::Out
                              ? completion_bonus(play.earned_prior + play.earned_score, play.form)
                              : 0;
        if (scores.paired) {
            score_row("TOTAL", base);
            score_row("ROUND 1", scores.earlier);
            score_row("ROUND 2", scores.latest);
        } else {
            score_row("SCORE", base);
        }
        if (play.finished && play.form != Form::Out) {
            score_row("BONUS", bonus);
        }
    };
    if (play.paused && !show_end) {
        glyph(frame, painter, 280.f + center_offset, 180.f, "PAUSED");
        menu_choice(frame, painter, 264.f + center_offset, 204.f, menu == 0, "RESUME");
        menu_choice(frame, painter, 264.f + center_offset, 220.f, menu == 1, "QUIT");
    } else if (show_end && play.form == Form::Out) {
        int row = 0;
        glyph(frame, painter, 288.f + center_offset, band(row++), "OUT");
        score_lines(row);
        menu_choice(frame, painter, 248.f + center_offset, band(row++), menu == 0, "RETRY");
        menu_choice(frame, painter, 248.f + center_offset, band(row++), menu == 1, "QUIT");
        if (assets != nullptr) {
            ScreenRect marks;
            marks.left = 480.f + center_offset;
            marks.top = 112.f;
            marks.right = 620.f + center_offset;
            marks.bottom = 204.f;
            draw_model_frame(segments, *assets, Slot::RankMarks, 0, marks);
        }
    } else if (show_end) {
        int row = 0;
        glyph(frame, painter, 248.f + center_offset, band(row++), "RESULTS");
        score_lines(row);
        glyph(frame, painter, 220.f + center_offset, band(row++), "FORM " + form_label(play.form));
        RoundJudgment earlier;
        earlier.perfects = prior_perfects;
        earlier.goods = prior_goods;
        earlier.misses = prior_misses;
        RoundJudgment latest;
        latest.perfects = play.perfects;
        latest.goods = play.goods;
        latest.misses = play.misses;
        const RoundJudgment counts = combined_judgment(earlier, latest, prior_score >= 0);
        glyph(frame,
              painter,
              220.f + center_offset,
              band(row++),
              "PERFECT " + std::to_string(counts.perfects));
        glyph(frame,
              painter,
              220.f + center_offset,
              band(row++),
              "GOOD " + std::to_string(counts.goods));
        glyph(frame,
              painter,
              220.f + center_offset,
              band(row++),
              "MISS " + std::to_string(counts.misses));
        menu_choice(frame, painter, 248.f + center_offset, band(row++), menu == 0, "RETRY");
        menu_choice(frame, painter, 248.f + center_offset, band(row++), menu == 1, "BACK");
        if (assets != nullptr) {
            ScreenRect marks;
            marks.left = 480.f + center_offset;
            marks.top = 328.f;
            marks.right = 620.f + center_offset;
            marks.bottom = 420.f;
            draw_model_frame(segments, *assets, Slot::RankMarks, 0, marks);
        }
    }
    if (segments.size() > stroked) {
        append_strokes(frame.triangles, std::span<const Segment>(segments).subspan(stroked));
    }
    return frame;
}

} // namespace oscilline
