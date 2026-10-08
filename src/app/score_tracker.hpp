// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors

#pragma once

#include "oscilline/render/stroke.hpp"

#include <cstdint>
#include <vector>

namespace oscilline {
class AssetRegistry;
struct CourseTimeline;

// A coupon's nominal on-screen height: the 50-unit symbol at the carousel's mean scale.
inline constexpr float kCouponHeightPx = 50.f * 0.44f;
// Carousel center line. The back of the semicircle rises 18 px and its coupon
// shrinks to 0.36, so the topmost edge sits one coupon height below the screen top.
inline constexpr float kCouponTrackY = kCouponHeightPx + 18.f + 25.f * 0.36f;

// One revolution per screen crossing, integrated across speed changes. The
// prelude (negative time) turns at the first obstacle's speed, so the carousel
// is already moving when the stage starts.
[[nodiscard]] double score_tracker_phase(const CourseTimeline& course, std::int64_t time_ms);

// Screen-space HUD. The course clock freezes carousel motion while paused.
[[nodiscard]] std::vector<Segment>
score_tracker_segments(int score, double phase, AssetRegistry* assets = nullptr);
} // namespace oscilline
