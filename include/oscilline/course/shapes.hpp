// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Original outlines for the ten obstacle kinds.

#pragma once

#include "oscilline/render/stroke.hpp"

#include <cstdint>
#include <vector>

namespace oscilline {

// Obstacle id of the loop. The outline is seated on the left ribbon foot of the cross.
inline constexpr std::uint8_t kLoopObstacleId = 2;
inline constexpr std::uint8_t kLoopPitObstacleId = 7;
inline constexpr std::uint8_t kLoopWaveObstacleId = 9;

// Proportions of the ten outlines. `loop` in obstacle_shape is the loop's
// width. Negative local y is up and the ribbon is y = 0. The pictures are
// original; they are not traced from a disc.

// Arch (block). Taller than it is wide. Width stays four fifths of the 0.68 arch.
// Height adds another tenth on the 1.10 ratio, then grows 5%.
// The left edge sits on the origin.
inline constexpr float kArchWidthFraction = 0.544f * 0.90f;
inline constexpr float kArchHeightToWidth = 1.10f * 1.10f * 1.05f;

// Spike (pit). A narrow V under the ribbon.
inline constexpr float kSpikeHalfWidthFraction = 0.22f;
inline constexpr float kSpikeDepthFraction = 0.50f;

// Loop. The cross sits above the ribbon, feet on y = 0, and the circle sits
// above the cross. The circle is open along the bottom arc between the cross
// intersections. Joins are half a side of the 16-gon (π/8).
inline constexpr float kLoopRadiusFraction = 0.5f;
// Standalone loops and loop-based pairs are 10% smaller. Block+loop keeps its size.
inline constexpr float kLoopVisualScale = 0.9f;
inline constexpr int kLoopSides = 16;
// sin(π/8) and cos(π/8). The joins land on a vertex.
inline constexpr float kLoopCrossSinPi8 = 0.38268343f;
inline constexpr float kLoopCrossCosPi8 = 0.92387953f;
// The previous cross reflected those joins through a point this far below the
// circle, in radii. That reflection's vertical span is
// 2 * (1 + drop - cos(π/8)).
inline constexpr float kLoopCrossDropFraction = 0.42f;
// Less than 1 pulls the ribbon ends up toward the joins, so the arms cover
// less vertical span.
inline constexpr float kLoopCrossSpanScale = 0.75f;
// Greater than 1 sets the feet farther from the center than the old reflection,
// so the bottom of the cross is wider.
inline constexpr float kLoopCrossFootScale = 1.5f;
inline constexpr float kLoopCrossSpanFraction =
    kLoopCrossSpanScale * 2.f * (1.f + kLoopCrossDropFraction - kLoopCrossCosPi8);
inline constexpr float kLoopCrossFootHalfFraction = kLoopCrossFootScale * kLoopCrossSinPi8;
// The joins sit `kLoopCrossSpanFraction` radii above the ribbon.
inline constexpr float kLoopRibbonLift = kLoopCrossSpanFraction + kLoopCrossCosPi8;
// Seating fraction: the left foot, as a fraction of the diameter from the leading
// edge. The outline is placed so that foot is the origin. The perfect window ends
// at the leading edge, not at this foot. The feet stay inside the circle, so the
// diameter is the outline's width.
inline constexpr float kLoopPerfectCenterFraction = (1.f - kLoopCrossFootHalfFraction) * 0.5f;

static_assert(kLoopCrossSpanScale < 1.f, "the cross arms cover less vertical span");
static_assert(kLoopCrossFootScale > 1.f, "the ribbon feet are wider than the old reflection");
static_assert(kLoopCrossFootHalfFraction < 1.f, "the feet stay inside the circle");

// Wave. A polygonal zig-zag: four peaks above the ribbon and three below,
// mirrored about the middle, starting and ending on the ribbon.
// Horizontal span, as a fraction of `loop`: 25% narrower than the previous span.
inline constexpr float kWaveWidthFraction = 1.20f * 0.80f * 0.75f;
inline constexpr float kWaveAmplitudeFraction = 0.22f;
inline constexpr int kWavePeaksAbove = 4;
inline constexpr int kWavePeaksBelow = 3;

// Block with a loop resting on its top. Head radius, as a fraction of `loop`.
// The circle's diameter equals the arch height.
inline constexpr float kBlockLoopHeadFraction = kArchWidthFraction * kArchHeightToWidth * 0.5f;

// Merged outline of obstacle `id` (0..9) in local space. The ribbon is y = 0
// and negative y is up. `loop_px` is the width of a loop. An unknown id is empty.
// The pictures are original; they are not traced from a disc.
[[nodiscard]] std::vector<Segment> obstacle_shape(std::uint8_t id, float loop_px);

// Width of that outline. Empty art is 0.
[[nodiscard]] float obstacle_width(std::uint8_t id, float loop_px);

// Leftmost local x of that outline. Rabbit meets this side first. Empty art is 0.
[[nodiscard]] float obstacle_leading_x(std::uint8_t id, float loop_px);

// Local x of the seated point. The loop uses the left ribbon foot of the cross
// (`kLoopPerfectCenterFraction` of its width). Every other kind uses the leading
// edge. Outlines are placed so this x is 0. The perfect window does not center
// here; see `obstacle_window`.
[[nodiscard]] float obstacle_perfect_anchor_x(std::uint8_t id, float loop_px);

// Placed x of the loop cross's left ribbon foot for the loop, loop+pit, and
// loop+wave. The loop's is 0; the pairs lead with their outline, so theirs is
// right of the origin. Other ids are 0.
[[nodiscard]] float obstacle_loop_foot_x(std::uint8_t id, float loop_px);

// Ids 4..9 are the merged pairs.
[[nodiscard]] inline bool obstacle_is_pair(std::uint8_t id) {
    return id >= 4 && id <= 9;
}

// A pair this wide is drawn at this fraction of `loop_px` when the next
// obstacle is closer than the outline. 1 leaves the art alone.
inline constexpr float kPairFitMargin = 0.85f;
inline constexpr float kMinPairFitScale = 0.35f;

[[nodiscard]] float pair_fit_scale(float width_px, float gap_px);

} // namespace oscilline
