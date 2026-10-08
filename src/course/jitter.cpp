// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Independent vertical offsets at ribbon breaks, held for 20 ms.

#include "oscilline/course/jitter.hpp"

#include "oscilline/course/shapes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

constexpr float kAttachEpsilon = 0.05f;
constexpr float kAttachSnap = 0.75f;

std::uint32_t mix32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::uint32_t jitter_hash(std::int64_t tick, std::uint32_t channel) {
    std::uint32_t x = static_cast<std::uint32_t>(tick) ^ 0x9e3779b9u;
    x += static_cast<std::uint32_t>(static_cast<std::uint64_t>(tick) >> 32) * 0x85ebca6bu;
    x ^= channel + 0xc2b2ae35u;
    return mix32(x);
}

std::int64_t jitter_tick(std::int64_t time_ms) {
    return ribbon_jitter_time(time_ms) / kRibbonJitterFrameMs;
}

std::uint32_t vertex_channel(float x, float y, std::uint32_t salt) {
    const auto qx = static_cast<std::uint32_t>(std::lround(x * 4.f));
    const auto qy = static_cast<std::uint32_t>(std::lround(y * 4.f));
    return mix32(qx * 0x9e3779b9u ^ qy * 0x85ebca6bu ^ salt * 0xc2b2ae35u);
}

std::uint32_t break_channel(float x) {
    return static_cast<std::uint32_t>(std::lround(x * 4.f)) ^ 0xB12Cu;
}

RibbonAttachment attachment_of(const std::vector<Segment>& shape) {
    RibbonAttachment attachment;
    bool any = false;
    float left = 0.f;
    float right = 0.f;
    const auto take = [&](float x) {
        if (!any) {
            left = x;
            right = x;
            any = true;
            return;
        }
        left = std::min(left, x);
        right = std::max(right, x);
    };
    for (const Segment& segment : shape) {
        if (std::fabs(segment.y0) <= kAttachEpsilon) {
            take(segment.x0);
        }
        if (std::fabs(segment.y1) <= kAttachEpsilon) {
            take(segment.x1);
        }
        const float dy = segment.y1 - segment.y0;
        const bool crosses =
            (segment.y0 > 0.f && segment.y1 < 0.f) || (segment.y0 < 0.f && segment.y1 > 0.f);
        if (crosses && std::fabs(dy) > 1.e-6f) {
            const float t = (0.f - segment.y0) / dy;
            take(segment.x0 + (segment.x1 - segment.x0) * t);
        }
    }
    if (!any || !(right > left + 1.e-3f)) {
        return attachment;
    }
    attachment.left = left;
    attachment.right = right;
    attachment.valid = true;
    return attachment;
}

float peak_ps(Form form, int hits) {
    if (form == Form::Worm) {
        return kRibbonJitterPeakWormPs;
    }
    if (hits >= 4) {
        return kRibbonJitterPeakHighPs;
    }
    return kRibbonJitterPeakLowPs;
}

float floor_ps(int hits) {
    if (hits >= 4) {
        return kRibbonJitterFloorHighPs;
    }
    return kRibbonJitterRestPs;
}

} // namespace

std::int64_t ribbon_jitter_time(std::int64_t time_ms) {
    constexpr std::int64_t kStep = kRibbonJitterFrameMs;
    if (time_ms >= 0) {
        return (time_ms / kStep) * kStep;
    }
    const std::int64_t steps = ((-time_ms) + kStep - 1) / kStep;
    return -steps * kStep;
}

float ribbon_jitter_amplitude_ps(Form form,
                                 int hits_since_form,
                                 std::int64_t last_hit_ms,
                                 std::int64_t now_ms) {
    if (hits_since_form <= 0 || last_hit_ms < 0) {
        return kRibbonJitterRestPs;
    }
    std::int64_t elapsed = ribbon_jitter_time(now_ms) - last_hit_ms;
    if (elapsed < 0) {
        elapsed = 0;
    }
    const float peak = peak_ps(form, hits_since_form);
    const float floor = floor_ps(hits_since_form);
    if (elapsed <= kRibbonJitterHoldMs) {
        return peak;
    }
    const std::int64_t fade_end =
        static_cast<std::int64_t>(kRibbonJitterHoldMs) + kRibbonJitterFadeMs;
    if (elapsed >= fade_end) {
        return floor;
    }
    const float u =
        static_cast<float>(elapsed - kRibbonJitterHoldMs) / static_cast<float>(kRibbonJitterFadeMs);
    return peak + (floor - peak) * u;
}

float ribbon_jitter_offset(std::int64_t time_ms, std::uint32_t channel, float amplitude) {
    if (!(amplitude > 0.f) || !std::isfinite(amplitude)) {
        return 0.f;
    }
    const std::uint32_t bits = jitter_hash(jitter_tick(time_ms), channel) & 0x00ffffffu;
    const float unit = static_cast<float>(bits) * (1.f / 16777216.f);
    return (unit * 2.f - 1.f) * amplitude;
}

float ribbon_break_y(float baseline_y, float x, std::int64_t time_ms, float amplitude_y) {
    return baseline_y + ribbon_jitter_offset(time_ms, break_channel(x), amplitude_y);
}

std::vector<Segment> ribbon_jitter_spine(float left,
                                         float right,
                                         float baseline_y,
                                         std::span<const float> attachment_x,
                                         std::int64_t time_ms,
                                         float amplitude_ps,
                                         float depth) {
    std::vector<Segment> out;
    if (!(right > left) || !std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(baseline_y)) {
        return out;
    }
    const float amplitude_y = std::max(amplitude_ps, 0.f) * kPlayStationPixelToLogicalY;
    std::vector<float> xs;
    xs.push_back(left);
    xs.push_back(right);
    for (float x : attachment_x) {
        if (std::isfinite(x) && x > left && x < right) {
            xs.push_back(x);
        }
    }
    std::sort(xs.begin(), xs.end());
    std::vector<float> breaks;
    breaks.reserve(xs.size());
    for (float x : xs) {
        if (breaks.empty() || x - breaks.back() > 1.e-3f) {
            breaks.push_back(x);
        }
    }
    for (std::size_t i = 1; i < breaks.size(); ++i) {
        Segment segment;
        segment.x0 = breaks[i - 1];
        segment.y0 = ribbon_break_y(baseline_y, breaks[i - 1], time_ms, amplitude_y);
        segment.x1 = breaks[i];
        segment.y1 = ribbon_break_y(baseline_y, breaks[i], time_ms, amplitude_y);
        segment.depth = depth;
        out.push_back(segment);
    }
    return out;
}

RibbonAttachment obstacle_ribbon_attachment(std::uint8_t id, float loop_px) {
    return attachment_of(obstacle_shape(id, loop_px));
}

void append_jittered_obstacle(std::vector<Segment>& out,
                              std::uint8_t id,
                              float origin_x,
                              float baseline_y,
                              float loop_px,
                              float scale,
                              std::int64_t time_ms,
                              float amplitude_ps,
                              std::uint32_t salt) {
    if (!(scale > 0.f) || !(loop_px > 0.f)) {
        return;
    }
    const std::vector<Segment> shape = obstacle_shape(id, loop_px * scale);
    if (shape.empty()) {
        return;
    }
    const RibbonAttachment attachment = attachment_of(shape);
    const float amplitude = std::max(amplitude_ps, 0.f);
    const float amplitude_x = amplitude * kPlayStationPixelToLogicalX;
    const float amplitude_y = amplitude * kPlayStationPixelToLogicalY;
    const float left_x = origin_x + attachment.left;
    const float right_x = origin_x + attachment.right;
    const float left_y = ribbon_break_y(baseline_y, left_x, time_ms, amplitude_y);
    const float right_y = ribbon_break_y(baseline_y, right_x, time_ms, amplitude_y);

    const auto map_point = [&](float x, float y) {
        if (attachment.valid && std::fabs(y) <= kAttachEpsilon) {
            const float to_left = std::fabs(x - attachment.left);
            const float to_right = std::fabs(x - attachment.right);
            if (to_left <= kAttachSnap && to_left <= to_right) {
                return std::pair{left_x, left_y};
            }
            if (to_right <= kAttachSnap) {
                return std::pair{right_x, right_y};
            }
        }
        const std::uint32_t channel = vertex_channel(x, y, salt);
        const float jx = ribbon_jitter_offset(time_ms, channel ^ 0xA11u, amplitude_x);
        const float jy = ribbon_jitter_offset(time_ms, channel ^ 0xB22u, amplitude_y);
        return std::pair{origin_x + x + jx, baseline_y + y + jy};
    };

    const auto emit = [&](float x0, float y0, float x1, float y1, const Rgb& color) {
        const auto [sx0, sy0] = map_point(x0, y0);
        const auto [sx1, sy1] = map_point(x1, y1);
        Segment segment;
        segment.x0 = sx0;
        segment.y0 = sy0;
        segment.x1 = sx1;
        segment.y1 = sy1;
        segment.depth = 1.f;
        segment.color = color;
        out.push_back(segment);
    };

    const auto emit_chain =
        [&](auto&& self, float x0, float y0, float x1, float y1, const Rgb& color) -> void {
        const float dy = y1 - y0;
        if (attachment.valid && std::fabs(dy) > 1.e-6f) {
            const float t = (0.f - y0) / dy;
            if (t > 0.001f && t < 0.999f) {
                const float ix = x0 + (x1 - x0) * t;
                float pin = 0.f;
                bool hit = false;
                if (std::fabs(ix - attachment.left) <= kAttachSnap) {
                    pin = attachment.left;
                    hit = true;
                } else if (std::fabs(ix - attachment.right) <= kAttachSnap) {
                    pin = attachment.right;
                    hit = true;
                }
                if (hit) {
                    self(self, x0, y0, pin, 0.f, color);
                    self(self, pin, 0.f, x1, y1, color);
                    return;
                }
            }
        }
        emit(x0, y0, x1, y1, color);
    };

    for (const Segment& segment : shape) {
        emit_chain(emit_chain, segment.x0, segment.y0, segment.x1, segment.y1, segment.color);
    }
}

void jitter_figure_vertices(std::span<Segment> segments,
                            std::int64_t time_ms,
                            float amplitude_ps,
                            std::uint32_t salt) {
    const float extra = (amplitude_ps - kRibbonJitterRestPs) * kFigureJitterScale;
    if (!(extra > 0.f) || segments.empty()) {
        return;
    }
    const float amplitude_x = extra * kPlayStationPixelToLogicalX;
    const float amplitude_y = extra * kPlayStationPixelToLogicalY;
    const auto shift = [&](float& x, float& y) {
        const std::uint32_t channel = vertex_channel(x, y, salt ^ 0xF16u);
        x += ribbon_jitter_offset(time_ms, channel ^ 0x100u, amplitude_x);
        y += ribbon_jitter_offset(time_ms, channel ^ 0x200u, amplitude_y);
    };
    for (Segment& segment : segments) {
        shift(segment.x0, segment.y0);
        shift(segment.x1, segment.y1);
    }
}

} // namespace oscilline
