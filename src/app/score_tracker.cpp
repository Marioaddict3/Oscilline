// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors

#include "score_tracker.hpp"

#include "oscilline/asset/registry.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/course/score.hpp"
#include "oscilline/render/project.hpp"

#include <algorithm>
#include <cmath>

namespace oscilline {
namespace {
constexpr float kTau = 6.283185307f;

// Original procedural fallback symbols; disc geometry is never bundled.
std::vector<ModelSegment> fallback_coupon(int shape) {
    std::vector<ModelSegment> out;
    const float hue = static_cast<float>(shape) / static_cast<float>(kCouponShapes);
    const Rgb color{0.65f + 0.35f * std::cos(kTau * hue),
                    0.65f + 0.35f * std::cos(kTau * (hue + 0.333f)),
                    0.65f + 0.35f * std::cos(kTau * (hue + 0.667f)),
                    1.f};
    const auto line = [&](float x0, float y0, float x1, float y1) {
        out.push_back({{x0, y0, 0.f}, {x1, y1, 0.f}, color});
    };
    if (shape == 0) {
        line(0.f, -18.f, 0.f, 18.f);
    } else if (shape == 1 || shape == 2) {
        const int arms = shape == 1 ? 3 : 4;
        for (int i = 0; i < arms; ++i) {
            const float a = kTau * static_cast<float>(i) / static_cast<float>(arms);
            line(0.f, 0.f, 25.f * std::cos(a), 25.f * std::sin(a));
        }
    } else {
        const int corners = 3 + (shape - 3) % 6;
        const bool star = shape >= 9;
        const int count = star ? corners * 2 : corners;
        for (int i = 0; i < count; ++i) {
            const auto point = [&](int index) {
                const float angle = kTau * static_cast<float>(index) / static_cast<float>(count);
                const float radius = star && index % 2 != 0 ? 12.f : 25.f;
                return ModelVertex{radius * std::cos(angle), radius * std::sin(angle), 0.f};
            };
            out.push_back({point(i), point((i + 1) % count), color});
        }
    }
    return out;
}
} // namespace

double score_tracker_phase(const CourseTimeline& course, std::int64_t time_ms) {
    int travel = static_cast<int>(2000 / kStageScrollSpeedScale);
    if (time_ms < 0) {
        // Phase is continuous through zero: run backwards at the opening speed.
        for (const auto& event : course.events) {
            if (const int mapped = stage_scroll_approach_ms(event); mapped > 0) {
                travel = mapped;
                break;
            }
        }
        return static_cast<double>(time_ms) / travel;
    }
    const auto end = time_ms;
    std::int64_t start = 0;
    double phase = 0.0;
    // Like the running character, follow the upcoming obstacle and retain the
    // last speed in the tail. Integrating avoids phase jumps at transitions.
    for (const auto& event : course.events) {
        const int mapped = stage_scroll_approach_ms(event);
        if (mapped > 0) {
            travel = mapped;
        }
        const auto boundary = std::clamp<std::int64_t>(event.hit_ms, start, end);
        phase += static_cast<double>(boundary - start) / travel;
        start = boundary;
        if (start == end) {
            return phase;
        }
    }
    return phase + static_cast<double>(end - start) / travel;
}

std::vector<Segment> score_tracker_segments(int score, double cycle_phase, AssetRegistry* assets) {
    const auto coupons = score_coupons(score);
    const TmdModel* model = assets != nullptr ? assets->model(Slot::ScoreCoupons) : nullptr;
    const auto* animations = assets != nullptr ? assets->animations(Slot::ScoreCoupons) : nullptr;
    const AnmFile* animation =
        animations != nullptr && !animations->empty() && !animations->front().frames.empty()
            ? &animations->front()
            : nullptr;
    const float clock = static_cast<float>(cycle_phase - std::floor(cycle_phase));
    const float center = static_cast<float>(logical_width()) * 0.5f;
    std::vector<Segment> out;
    for (int slot = 0; slot < kCouponCount; ++slot) {
        const int shape = coupons[static_cast<std::size_t>(slot)];
        // First and last coupons sit at opposite ends of a rotating semicircle.
        const float phase = static_cast<float>(slot) / (2.f * (kCouponCount - 1)) - clock;
        const float angle = phase * kTau;
        float x = center + 125.f * std::sin(angle);
        float y = kCouponTrackY - 18.f * std::cos(angle);
        float scale = 0.44f + 0.08f * std::cos(angle);
        float rotation = -angle * 2.f;
        float depth = -std::cos(angle);
        if (animation != nullptr) {
            const float frame =
                (phase - std::floor(phase)) * static_cast<float>(animation->frames.size());
            const auto poses = poses_for_frame(
                *animation, 1, static_cast<int>(frame), frame - std::floor(frame), true);
            if (!poses.empty() && poses.front().visible) {
                const Pose& pose = poses.front();
                x = center + pose.position_x * 0.37f;
                y = kCouponTrackY + 18.f + (pose.position_y + 484.f) * 0.36f;
                scale = pose.scale_x * 0.4f;
                rotation = pose.rotation_z;
                depth = pose.position_z;
            }
        }
        std::vector<ModelSegment> lines;
        if (model != nullptr && model->objects.size() >= kCouponShapes) {
            std::vector<Pose> poses(model->objects.size());
            poses[static_cast<std::size_t>(shape)].visible = true;
            WireframeOptions options;
            options.packet_color = true;
            lines = tmd_wireframe(*model, poses, options);
        }
        if (lines.empty()) {
            lines = fallback_coupon(shape);
        }
        const float c = std::cos(rotation), s = std::sin(rotation);
        for (const auto& line : lines) {
            const auto project = [&](ModelVertex point) {
                return ModelVertex{x + scale * (point.x * c - point.y * s),
                                   y + scale * (point.x * s + point.y * c),
                                   depth};
            };
            const auto a = project(line.a), b = project(line.b);
            out.push_back({a.x, a.y, b.x, b.y, depth, line.color});
        }
    }
    return out;
}
} // namespace oscilline
