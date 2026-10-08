// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors

#include "oscilline/course/score.hpp"

#include <algorithm>
#include <bit>

namespace oscilline {

int coupon_value(int shape, int position) {
    if (shape <= 0 || shape >= kCouponShapes || position < 0 || position >= kCouponCount) {
        return 0;
    }
    const int n = shape + 6 - position;
    const int k = 7 - position;
    int value = 1;
    for (int i = 1; i <= k; ++i) {
        value = value * (n - k + i) / i;
    }
    return value;
}

ScoreCoupons score_coupons(int score) {
    score = std::clamp(score, 0, kMaxCouponScore);
    ScoreCoupons coupons{};
    for (int position = 0; position < kCouponCount; ++position) {
        int shape = kCouponShapes - 1;
        while (shape > 0 && coupon_value(shape, position) > score) {
            --shape;
        }
        coupons[static_cast<std::size_t>(position)] = shape;
        score -= coupon_value(shape, position);
    }
    return coupons;
}

int coupons_score(const ScoreCoupons& coupons) {
    int score = 0;
    for (int position = 0; position < kCouponCount; ++position) {
        score += coupon_value(coupons[static_cast<std::size_t>(position)], position);
    }
    return score;
}

int clear_score_points(int streak, Form form, Judgment judgment, std::uint8_t freestyle) {
    if (judgment != Judgment::Perfect && judgment != Judgment::Good) {
        return 0;
    }
    const int base = std::clamp(streak, 1, kScoreStreakCap) * form_score_multiplier(form);
    const int accuracy = judgment == Judgment::Perfect ? kScorePerfect : kScoreGood;
    return base + accuracy + std::popcount(static_cast<unsigned>(freestyle & 15));
}

int completion_bonus(int earned_score, Form form) {
    int percent = 0;
    switch (form) {
    case Form::Super:
        percent = 30;
        break;
    case Form::Rabbit:
        percent = 20;
        break;
    case Form::Frog:
        percent = 15;
        break;
    case Form::Worm:
    case Form::Out:
        break;
    }
    return static_cast<int>(
        static_cast<std::int64_t>(std::clamp(earned_score, 0, kMaxEarnedScore)) * percent / 100);
}

int result_score(const PlayState& play) {
    return play.score + (play.finished && play.form != Form::Out
                             ? completion_bonus(play.earned_prior + play.earned_score, play.form)
                             : 0);
}

void carry_score_into(PlayState& next, const PlayState& previous) {
    const int total = previous.coupon_prior + result_score(previous);
    next.coupon_prior = std::min(kMaxCouponScore, total);
    const int bonus =
        previous.finished && previous.form != Form::Out
            ? completion_bonus(previous.earned_prior + previous.earned_score, previous.form)
            : 0;
    next.earned_prior = static_cast<int>(std::min<std::int64_t>(
        kMaxEarnedScore,
        static_cast<std::int64_t>(previous.earned_prior) + previous.earned_score + bonus));
}

} // namespace oscilline
