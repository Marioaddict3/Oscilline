// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors

#pragma once

#include "oscilline/course/play.hpp"

#include <array>
#include <cstdint>

namespace oscilline {

inline constexpr int kCouponCount = 7;
inline constexpr int kCouponShapes = 15;
inline constexpr int kMaxCouponScore = 116279;
// Avoid the original game's signed overflow on exceptionally long custom tracks.
inline constexpr int kMaxEarnedScore = 1000000000;
using ScoreCoupons = std::array<int, kCouponCount>;

// Position 0 is the most significant coupon. Values are the wiki's table,
// C(shape + 6 - position, 7 - position), with shape 0 worth zero.
[[nodiscard]] int coupon_value(int shape, int position);
[[nodiscard]] ScoreCoupons score_coupons(int score);
[[nodiscard]] int coupons_score(const ScoreCoupons& coupons);

[[nodiscard]] int
clear_score_points(int streak, Form form, Judgment judgment, std::uint8_t freestyle = 0);
[[nodiscard]] int completion_bonus(int earned_score, Form form);
[[nodiscard]] int result_score(const PlayState& play);
// Start the next course with the previous stage bonus already in its coupons.
void carry_score_into(PlayState& next, const PlayState& previous);

} // namespace oscilline
