// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Original arch, spike, loop, zig-zag wave, and the six pair outlines.

#include "oscilline/course/shapes.hpp"

#include "oscilline/course/camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace oscilline {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
// Upper ends of the X, measured out from the bottom of the circle. 22.5°
// lands on a vertex of the 16-sided loop.
constexpr float kCrossPhi = kPi / 8.f;
// One clear triangle on loop+pit, and the shorter spikes around wave+loop.
constexpr float kTopSpikeHeightFraction = 0.75f;
constexpr float kTopSpikeHalfAngle = 0.58f;
constexpr float kRingSpikeLengthFraction = 0.36f;
constexpr float kRingSpikeHalfAngle = 0.16f;
// A tooth on each side of wave+pit, as a fraction of the pit's half-width.
constexpr float kPitToothFraction = 0.80f;
// Keep a pit cut into a block's roof off the ribbon.
constexpr float kNotchHeightLimit = 0.92f;

static_assert(kLoopSides == 16, "the cross phi is half a side, so it meets a vertex");
static_assert(kWavePeaksAbove == kWavePeaksBelow + 1, "the zig-zag is a palindrome");

struct Vec {
    float x = 0.f;
    float y = 0.f;
};

void add(std::vector<Segment>& out, float x0, float y0, float x1, float y1) {
    Segment segment;
    segment.x0 = x0;
    segment.y0 = y0;
    segment.x1 = x1;
    segment.y1 = y1;
    out.push_back(segment);
}

void add(std::vector<Segment>& out, Vec a, Vec b) {
    add(out, a.x, a.y, b.x, b.y);
}

void poly(std::vector<Segment>& out, const float* xy, std::size_t points) {
    for (std::size_t i = 1; i < points; ++i) {
        add(out, xy[(i - 1) * 2], xy[(i - 1) * 2 + 1], xy[i * 2], xy[i * 2 + 1]);
    }
}

Vec on_circle(float cx, float cy, float radius, float theta) {
    return {cx + std::cos(theta) * radius, cy - std::sin(theta) * radius};
}

float wrap_angle(float angle) {
    const float turn = kPi * 2.f;
    while (angle < 0.f) {
        angle += turn;
    }
    while (angle >= turn) {
        angle -= turn;
    }
    return angle;
}

// A counterclockwise gap in the circle. `from` and `to` are wrapped angles.
struct OpenArc {
    float from = 0.f;
    float to = 0.f;
};

OpenArc open_arc(float from, float to) {
    return {wrap_angle(from), wrap_angle(to)};
}

bool angle_in_closed_arc(float angle, OpenArc arc) {
    constexpr float kSlop = 1.e-4f;
    if (arc.from <= arc.to) {
        return angle + kSlop >= arc.from && angle - kSlop <= arc.to;
    }
    return angle + kSlop >= arc.from || angle - kSlop <= arc.to;
}

struct Extent {
    float lo = 0.f;
    float hi = 0.f;
    bool any = false;
};

Extent extent_of(const std::vector<Segment>& shape) {
    Extent extent;
    if (shape.empty()) {
        return extent;
    }
    extent.any = true;
    extent.lo = shape.front().x0;
    extent.hi = extent.lo;
    for (const Segment& segment : shape) {
        extent.lo = std::min(extent.lo, std::min(segment.x0, segment.x1));
        extent.hi = std::max(extent.hi, std::max(segment.x0, segment.x1));
    }
    return extent;
}

// Slide the outline so `fraction` of the way from the left edge sits on x = 0.
void place(std::vector<Segment>& shape, float fraction) {
    const Extent extent = extent_of(shape);
    if (!extent.any) {
        return;
    }
    const float anchor = extent.lo + fraction * (extent.hi - extent.lo);
    if (anchor == 0.f) {
        return;
    }
    for (Segment& segment : shape) {
        segment.x0 -= anchor;
        segment.x1 -= anchor;
    }
}

struct LoopGeom {
    float cx = 0.f;
    float cy = 0.f;
    float radius = 0.f;
};

LoopGeom loop_geom(float loop) {
    const float radius = loop * kLoopRadiusFraction;
    return {0.f, -kLoopRibbonLift * radius, radius};
}

// `open` drops the arcs between a cross or a spike's two joins, so the outline
// runs out along that feature instead of drawing a chord across its base.
void add_circle(std::vector<Segment>& out,
                LoopGeom loop,
                const std::vector<float>& extra,
                const std::vector<OpenArc>& open) {
    std::vector<float> angles;
    angles.reserve(static_cast<std::size_t>(kLoopSides) + extra.size() + 4);
    for (int i = 0; i < kLoopSides; ++i) {
        angles.push_back(kPi * 2.f * static_cast<float>(i) / static_cast<float>(kLoopSides));
    }
    // Cardinals, so the width is the diameter and the crown and the bottom are exact.
    angles.push_back(0.f);
    angles.push_back(kPi * 0.5f);
    angles.push_back(kPi);
    angles.push_back(kPi * 1.5f);
    for (float angle : extra) {
        angles.push_back(wrap_angle(angle));
    }
    std::sort(angles.begin(), angles.end());
    std::vector<float> unique;
    unique.reserve(angles.size());
    for (float angle : angles) {
        if (unique.empty() || angle - unique.back() > 1.e-4f) {
            unique.push_back(angle);
        }
    }
    const std::size_t count = unique.size();
    for (std::size_t i = 0; i < count; ++i) {
        const float from = unique[i];
        const float to = unique[(i + 1) % count];
        bool skip = false;
        for (const OpenArc& arc : open) {
            if (angle_in_closed_arc(from, arc) && angle_in_closed_arc(to, arc)) {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }
        const Vec a = on_circle(loop.cx, loop.cy, loop.radius, from);
        const Vec b = on_circle(loop.cx, loop.cy, loop.radius, to);
        add(out, a, b);
    }
}

// Two arms from the circle joins to the opposite feet on the ribbon. The feet
// are `kLoopCrossFootScale` times as far apart as the old reflection, and the
// joins sit `kLoopCrossSpanScale` of that reflection's height above the ribbon,
// so the cross is shorter and wider.
void add_cross(std::vector<Segment>& out, LoopGeom loop) {
    const float bottom = -kPi * 0.5f;
    const Vec left = on_circle(loop.cx, loop.cy, loop.radius, bottom - kCrossPhi);
    const Vec right = on_circle(loop.cx, loop.cy, loop.radius, bottom + kCrossPhi);
    const float foot = loop.radius * kLoopCrossFootHalfFraction;
    const Vec foot_left{loop.cx - foot, 0.f};
    const Vec foot_right{loop.cx + foot, 0.f};
    add(out, left, foot_right);
    add(out, right, foot_left);
}

std::vector<float> cross_angles() {
    const float bottom = -kPi * 0.5f;
    return {bottom - kCrossPhi, bottom + kCrossPhi};
}

OpenArc cross_open_arc() {
    const float bottom = -kPi * 0.5f;
    return open_arc(bottom - kCrossPhi, bottom + kCrossPhi);
}

void add_top_spike(std::vector<Segment>& out, LoopGeom loop) {
    const float top = kPi * 0.5f;
    const Vec left = on_circle(loop.cx, loop.cy, loop.radius, top + kTopSpikeHalfAngle);
    const Vec right = on_circle(loop.cx, loop.cy, loop.radius, top - kTopSpikeHalfAngle);
    const Vec apex{loop.cx, (loop.cy - loop.radius) - loop.radius * kTopSpikeHeightFraction};
    add(out, left, apex);
    add(out, apex, right);
}

std::vector<float> top_spike_angles() {
    const float top = kPi * 0.5f;
    return {top - kTopSpikeHalfAngle, top + kTopSpikeHalfAngle};
}

// Spikes around the loop, kept clear of the bottom cross.
void add_ring_spikes(std::vector<Segment>& out, LoopGeom loop) {
    const float dirs[] = {
        kPi * 0.5f, kPi * 0.5f + 0.78f, kPi * 0.5f - 0.78f, kPi, 0.f, kPi + 0.78f, -0.78f};
    const float reach = loop.radius * (1.f + kRingSpikeLengthFraction);
    for (float dir : dirs) {
        const Vec left = on_circle(loop.cx, loop.cy, loop.radius, dir + kRingSpikeHalfAngle);
        const Vec right = on_circle(loop.cx, loop.cy, loop.radius, dir - kRingSpikeHalfAngle);
        const Vec apex = on_circle(loop.cx, loop.cy, reach, dir);
        add(out, left, apex);
        add(out, apex, right);
    }
}

std::vector<float> ring_spike_angles() {
    const float dirs[] = {
        kPi * 0.5f, kPi * 0.5f + 0.78f, kPi * 0.5f - 0.78f, kPi, 0.f, kPi + 0.78f, -0.78f};
    std::vector<float> angles;
    angles.reserve(14);
    for (float dir : dirs) {
        angles.push_back(dir - kRingSpikeHalfAngle);
        angles.push_back(dir + kRingSpikeHalfAngle);
    }
    return angles;
}

std::vector<OpenArc> ring_spike_open_arcs() {
    const float dirs[] = {
        kPi * 0.5f, kPi * 0.5f + 0.78f, kPi * 0.5f - 0.78f, kPi, 0.f, kPi + 0.78f, -0.78f};
    std::vector<OpenArc> arcs;
    arcs.reserve(7);
    for (float dir : dirs) {
        arcs.push_back(open_arc(dir - kRingSpikeHalfAngle, dir + kRingSpikeHalfAngle));
    }
    return arcs;
}

float arch_height(float loop) {
    return loop * kArchWidthFraction * kArchHeightToWidth;
}

void arch(std::vector<Segment>& out, float loop) {
    const float half = loop * kArchWidthFraction * 0.5f;
    const float top = -arch_height(loop);
    const float xy[] = {-half, 0.f, -half, top, half, top, half, 0.f};
    poly(out, xy, 4);
}

void spike(std::vector<Segment>& out, float loop) {
    const float half = loop * kSpikeHalfWidthFraction;
    const float depth = loop * kSpikeDepthFraction;
    const float xy[] = {-half, 0.f, 0.f, depth, half, 0.f};
    poly(out, xy, 3);
}

// Straight runs between the extrema. Ends sit on y_mid. Even peaks go up.
void add_zigzag(std::vector<Segment>& out, float x0, float x1, float y_mid, float amplitude) {
    const int extrema = kWavePeaksAbove + kWavePeaksBelow;
    const float step = (x1 - x0) / static_cast<float>(extrema);
    float prev_x = x0;
    float prev_y = y_mid;
    for (int i = 0; i < extrema; ++i) {
        const float x = x0 + step * (static_cast<float>(i) + 0.5f);
        const float y = y_mid + ((i % 2 == 0) ? -amplitude : amplitude);
        add(out, prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }
    add(out, prev_x, prev_y, x1, y_mid);
}

void loop_shape(std::vector<Segment>& out, float loop) {
    const LoopGeom geom = loop_geom(loop);
    add_circle(out, geom, cross_angles(), {cross_open_arc()});
    add_cross(out, geom);
}

void wave(std::vector<Segment>& out, float loop) {
    const float half = loop * kWaveWidthFraction * 0.5f;
    add_zigzag(out, -half, half, 0.f, loop * kWaveAmplitudeFraction);
}

// Vertical sides. The whole top edge is the pit, pointing down into the block.
void block_pit(std::vector<Segment>& out, float loop) {
    const float half = loop * kArchWidthFraction * 0.5f;
    const float height = arch_height(loop);
    const float top = -height;
    const float notch = std::min(loop * kSpikeDepthFraction, height * kNotchHeightLimit);
    const float xy[] = {-half, 0.f, -half, top, 0.f, top + notch, half, top, half, 0.f};
    poly(out, xy, 5);
}

// The arch, with a circle on the middle of its roof. The circle is as tall as the arch.
void block_loop(std::vector<Segment>& out, float loop) {
    arch(out, loop);
    const float head = loop * kBlockLoopHeadFraction;
    const float cy = -arch_height(loop) - head;
    const LoopGeom geom{0.f, cy, head};
    add_circle(out, geom, {kPi * 1.5f}, {});
}

// Vertical sides. The top edge is the same zig-zag as a wave.
void block_wave(std::vector<Segment>& out, float loop) {
    const float half = loop * kArchWidthFraction * kPairWidthScale * 0.5f;
    const float top = -arch_height(loop);
    const float amplitude = std::min(loop * kWaveAmplitudeFraction, arch_height(loop) * 0.75f);
    add(out, -half, 0.f, -half, top);
    add_zigzag(out, -half, half, top, amplitude);
    add(out, half, top, half, 0.f);
}

// The loop, with one triangular spike out of the crown.
void loop_pit(std::vector<Segment>& out, float loop) {
    const LoopGeom geom = loop_geom(loop);
    std::vector<float> angles = cross_angles();
    const std::vector<float> spike = top_spike_angles();
    angles.insert(angles.end(), spike.begin(), spike.end());
    std::vector<OpenArc> open{cross_open_arc()};
    const float top = kPi * 0.5f;
    open.push_back(open_arc(top - kTopSpikeHalfAngle, top + kTopSpikeHalfAngle));
    add_circle(out, geom, angles, open);
    add_cross(out, geom);
    add_top_spike(out, geom);
}

// The pit, with a tooth sticking out of each side.
void pit_wave(std::vector<Segment>& out, float loop) {
    const float half = loop * kSpikeHalfWidthFraction;
    const float depth = loop * kSpikeDepthFraction;
    const float tooth = half * kPitToothFraction;
    const float xy[] = {-half,
                        0.f,
                        -half - tooth,
                        depth * 0.34f,
                        -half * 0.38f,
                        depth * 0.62f,
                        0.f,
                        depth,
                        half * 0.38f,
                        depth * 0.62f,
                        half + tooth,
                        depth * 0.34f,
                        half,
                        0.f};
    poly(out, xy, 7);
}

// The loop's circle and its cross, with spikes joined into the outline.
// Each spike replaces the arc between its joins, so no chord separates it.
void loop_wave(std::vector<Segment>& out, float loop) {
    const LoopGeom geom = loop_geom(loop);
    std::vector<float> angles = cross_angles();
    const std::vector<float> spikes = ring_spike_angles();
    angles.insert(angles.end(), spikes.begin(), spikes.end());
    std::vector<OpenArc> open = ring_spike_open_arcs();
    open.push_back(cross_open_arc());
    add_circle(out, geom, angles, open);
    add_cross(out, geom);
    add_ring_spikes(out, geom);
}

} // namespace

namespace {
// The outline before placement. Loop-family art is centered on the circle.
std::vector<Segment> raw_obstacle_shape(std::uint8_t id, float loop_px) {
    std::vector<Segment> out;
    if (!(loop_px > 0.f)) {
        return out;
    }
    const float loop_visual =
        (id == 2 || id == 7 || id == 9) ? loop_px * kLoopVisualScale : loop_px;
    switch (id) {
    case 0:
        arch(out, loop_px);
        break;
    case 1:
        spike(out, loop_px);
        break;
    case 2:
        loop_shape(out, loop_visual);
        break;
    case 3:
        wave(out, loop_px);
        break;
    case 4:
        block_pit(out, loop_px);
        break;
    case 5:
        block_loop(out, loop_px);
        break;
    case 6:
        block_wave(out, loop_px);
        break;
    case 7:
        loop_pit(out, loop_visual);
        break;
    case 8:
        pit_wave(out, loop_px);
        break;
    case 9:
        loop_wave(out, loop_visual);
        break;
    default:
        break;
    }
    return out;
}

// Placement fraction: the loop's left ribbon foot, or every other leading edge.
float placement_fraction(std::uint8_t id) {
    return id == kLoopObstacleId ? kLoopPerfectCenterFraction : 0.f;
}
} // namespace

std::vector<Segment> obstacle_shape(std::uint8_t id, float loop_px) {
    std::vector<Segment> out = raw_obstacle_shape(id, loop_px);
    // The loop's left ribbon foot sits on the origin. Every other outline puts
    // its leading edge there.
    place(out, placement_fraction(id));
    return out;
}

namespace {
// Placed extent and loop-foot offset of one outline at one size.
struct OutlineMetrics {
    float lo = 0.f;
    float hi = 0.f;
    float foot = 0.f;
};

OutlineMetrics measure_outline(std::uint8_t id, float loop_px) {
    OutlineMetrics metrics;
    const Extent placed = extent_of(obstacle_shape(id, loop_px));
    if (placed.any) {
        metrics.lo = placed.lo;
        metrics.hi = placed.hi;
    }
    if (id == kLoopObstacleId || id == kLoopPitObstacleId || id == kLoopWaveObstacleId) {
        // The three share the loop's circle and cross, centered on x = 0 before
        // placement, so the foot's raw x comes from the plain loop.
        const Extent loop = extent_of(raw_obstacle_shape(kLoopObstacleId, loop_px));
        const Extent own = extent_of(raw_obstacle_shape(id, loop_px));
        if (loop.any && own.any) {
            const float foot = loop.lo + kLoopPerfectCenterFraction * (loop.hi - loop.lo);
            metrics.foot = foot - (own.lo + placement_fraction(id) * (own.hi - own.lo));
        }
    }
    return metrics;
}

// Outlines depend only on (id, loop_px), and hit windows, cameras, and drawing
// ask for them every frame. Each thread keeps the last size measured per id.
OutlineMetrics outline_metrics(std::uint8_t id, float loop_px) {
    constexpr std::size_t kCachedIds = 10;
    struct Entry {
        float loop_px = -1.f;
        OutlineMetrics metrics;
    };
    thread_local std::array<Entry, kCachedIds> cache;
    if (id >= kCachedIds) {
        return measure_outline(id, loop_px);
    }
    Entry& entry = cache[id];
    if (entry.loop_px != loop_px) {
        entry.metrics = measure_outline(id, loop_px);
        entry.loop_px = loop_px;
    }
    return entry.metrics;
}

void obstacle_extent(std::uint8_t id, float loop_px, float& lo, float& hi) {
    const OutlineMetrics metrics = outline_metrics(id, loop_px);
    lo = metrics.lo;
    hi = metrics.hi;
}
} // namespace

float obstacle_loop_foot_x(std::uint8_t id, float loop_px) {
    return outline_metrics(id, loop_px).foot;
}

float obstacle_width(std::uint8_t id, float loop_px) {
    float lo = 0.f;
    float hi = 0.f;
    obstacle_extent(id, loop_px, lo, hi);
    return hi - lo;
}

float obstacle_leading_x(std::uint8_t id, float loop_px) {
    float lo = 0.f;
    float hi = 0.f;
    obstacle_extent(id, loop_px, lo, hi);
    return lo;
}

float obstacle_perfect_anchor_x(std::uint8_t id, float loop_px) {
    float lo = 0.f;
    float hi = 0.f;
    obstacle_extent(id, loop_px, lo, hi);
    if (!(hi > lo)) {
        return 0.f;
    }
    if (id == kLoopObstacleId) {
        return lo + kLoopPerfectCenterFraction * (hi - lo);
    }
    return lo;
}

float pair_fit_scale(float width_px, float gap_px) {
    if (!(width_px > 0.f)) {
        return 1.f;
    }
    if (!(gap_px > 0.f)) {
        return kMinPairFitScale;
    }
    if (gap_px >= width_px) {
        return 1.f;
    }
    const float scaled = gap_px * kPairFitMargin / width_px;
    if (scaled < kMinPairFitScale) {
        return kMinPairFitScale;
    }
    if (scaled > 1.f) {
        return 1.f;
    }
    return scaled;
}

} // namespace oscilline
