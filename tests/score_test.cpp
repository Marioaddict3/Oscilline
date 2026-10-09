// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors

#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/score.hpp"
#include "oscilline/render/viewport.hpp"
#include "score_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>

using namespace oscilline;

TEST_CASE("coupon table row totals match the documented thresholds") {
    constexpr int totals[] = {
        0, 7, 35, 119, 329, 791, 1715, 3431, 6434, 11439, 19447, 31823, 50387, 77519, 116279};
    for (int shape = 0; shape < kCouponShapes; ++shape) {
        ScoreCoupons coupons;
        coupons.fill(shape);
        CHECK(coupons_score(coupons) == totals[shape]);
        CHECK(score_coupons(totals[shape]) == coupons);
    }
    CHECK(coupon_value(2, 0) == 8);
    CHECK(coupon_value(2, 6) == 2);
    CHECK(coupon_value(14, 0) == 77520);
}

TEST_CASE("every representable score round trips through seven coupons") {
    for (int score = 0; score <= kMaxCouponScore; ++score) {
        const auto coupons = score_coupons(score);
        REQUIRE(coupons_score(coupons) == score);
        REQUIRE(std::all_of(coupons.begin(), coupons.end(), [](int shape) {
            return shape >= 0 && shape < kCouponShapes;
        }));
    }
    CHECK(score_coupons(-1) == score_coupons(0));
    CHECK(score_coupons(kMaxCouponScore + 1) == score_coupons(kMaxCouponScore));
    CHECK(coupons_score(score_coupons(730)) == 730);
}

TEST_CASE("streak scoring caps at eleven and only super doubles the base") {
    for (Form form : {Form::Rabbit, Form::Frog, Form::Worm}) {
        CHECK(clear_score_points(1, form, Judgment::Good) == 2);
        CHECK(clear_score_points(1, form, Judgment::Perfect) == 3);
        CHECK(clear_score_points(99, form, Judgment::Perfect) == 13);
    }
    CHECK(clear_score_points(11, Form::Super, Judgment::Perfect) == 24);
    CHECK(clear_score_points(11, Form::Super, Judgment::Good) == 23);
    CHECK(clear_score_points(1, Form::Rabbit, Judgment::Perfect, kActionLoop | kActionPit) == 5);
    CHECK(clear_score_points(11, Form::Super, Judgment::Miss, 15) == 0);
}

TEST_CASE("misses discard freestyle and scoring streak while promotion preserves scoring streak") {
    CourseTimeline course;
    course.duration_ms = 30000;
    for (int i = 0; i < 24; ++i) {
        CourseEvent event;
        event.obstacle = 0;
        event.hit_ms = 1000 + i * 1000;
        course.events.push_back(event);
    }
    PlayState play;
    const auto hit = [&](int index, bool perfect) {
        const auto w = obstacle_window(course.events[static_cast<std::size_t>(index)]);
        const auto time = perfect ? (w.perfect_open + w.perfect_close) / 2 : w.good_open;
        play_advance(play, course, time - 1, time, kActionBlock);
    };
    const auto first = obstacle_window(course.events[0]);
    play_advance(play, course, 0, first.good_open - 100, kActionLoop);
    play_advance(play, course, first.good_open - 100, first.good_open - 50, kActionLoop);
    CHECK(play.score == 0);
    hit(0, true);
    CHECK(play.score == 4); // Duplicate freestyle action is credited once.
    hit(1, true);
    CHECK(play.score == 8);
    for (int i = 2; i < 18; ++i) {
        hit(i, true);
    }
    CHECK(play.form == Form::Super);
    CHECK(play.clear_run == 0);
    CHECK(play.score_streak == 11);
    const int before = play.score;
    hit(18, true);
    CHECK(play.score == before + 24);
    const auto miss = obstacle_window(course.events[19]);
    play_advance(play, course, miss.good_open - 20, miss.good_open - 10, kActionLoop);
    play_advance(play, course, miss.good_close, miss.good_close + 1, 0);
    CHECK(play.score_streak == 0);
    CHECK(play.freestyle == 0);
    CHECK(play.score == before + 24);
    hit(20, false);
    CHECK(play.score == before + 26);
    const int frozen = play.score;
    play.paused = true;
    hit(21, true);
    CHECK(play.score == frozen);
}

TEST_CASE("clear bonuses round down and failure and unfinished runs earn none") {
    CHECK(completion_bonus(101, Form::Super) == 30);
    CHECK(completion_bonus(101, Form::Rabbit) == 20);
    CHECK(completion_bonus(101, Form::Frog) == 15);
    CHECK(completion_bonus(101, Form::Worm) == 0);
    PlayState play;
    play.score = play.earned_score = 101;
    CHECK(result_score(play) == 101);
    play.finished = true;
    CHECK(result_score(play) == 121);
    play.form = Form::Out;
    CHECK(result_score(play) == 101);
}

TEST_CASE("coupons saturate but earned totals continue supplying the bonus") {
    CourseTimeline course;
    course.duration_ms = 4000;
    CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 1000;
    course.events.push_back(event);
    PlayState play;
    play.score = kMaxCouponScore - 1;
    play.earned_score = kMaxCouponScore - 1;
    const auto window = obstacle_window(event);
    play_advance(play, course, 0, window.perfect_open, kActionBlock);
    CHECK(play.score == kMaxCouponScore);
    CHECK(play.earned_score == kMaxCouponScore + 2);
    play_advance(play, course, 1000, course.duration_ms, 0);
    CHECK(play.finished);
    CHECK(result_score(play) == kMaxCouponScore + (kMaxCouponScore + 2) / 5);
    const int final = result_score(play);
    play_advance(play, course, 4000, 5000, kActionBlock);
    CHECK(result_score(play) == final);
    PlayState next;
    carry_score_into(next, play);
    CHECK(next.coupon_prior == kMaxCouponScore);
    play_advance(next, course, 0, window.perfect_open, kActionBlock);
    CHECK(next.score == 0);
    CHECK(next.earned_score == 3);
}

TEST_CASE("round two carries scoring streak and cumulative coupon cap without duplicate bonus") {
    PlayState previous;
    previous.score = 100;
    previous.earned_score = 100;
    previous.score_streak = 11;
    previous.form = Form::Super;
    previous.finished = true;
    PlayState next;
    carry_form_into(next, previous, true);
    CHECK(next.score == 0);
    CHECK(next.score_streak == 11);
    carry_score_into(next, previous);
    CHECK(next.coupon_prior == 130);
    CHECK(next.earned_prior == 130);
    next.score = next.earned_score = 50;
    next.finished = true;
    CHECK(result_score(next) + result_score(previous) == 234);
    PlayState retry;
    carry_form_into(retry, previous, false);
    CHECK(retry.coupon_prior == 0);
    CHECK(retry.score_streak == 0);
}

TEST_CASE("coupon tracker sits one coupon height below the top and stays centered") {
    set_logical_width(kLogicalWidth);
    const auto normal = score_tracker_segments(730, 0.125);
    REQUIRE_FALSE(normal.empty());
    set_logical_width(960);
    const auto wide = score_tracker_segments(730, 0.125);
    set_logical_width(kLogicalWidth);
    REQUIRE(wide.size() == normal.size());
    for (std::size_t i = 0; i < normal.size(); ++i) {
        CHECK(wide[i].x0 - normal[i].x0 == doctest::Approx(160.f));
        CHECK(wide[i].y0 == doctest::Approx(normal[i].y0));
        CHECK(normal[i].y0 >= kCouponHeightPx - 0.01f);
        CHECK(normal[i].y0 < 100.f);
        CHECK(std::isfinite(normal[i].x0));
    }
    const auto moved = score_tracker_segments(730, 0.25);
    CHECK(moved.front().x0 != doctest::Approx(normal.front().x0));
    CHECK(score_tracker_segments(730, 1.125).front().x0 == doctest::Approx(normal.front().x0));

    // At a quarter-cycle, the first coupon has traveled left from the top.
    const auto reversed = score_tracker_segments(0, 0.25);
    REQUIRE(reversed.size() == kCouponCount);
    CHECK((reversed.front().x0 + reversed.front().x1) / 2 ==
          doctest::Approx(logical_width() * 0.5 - 125.f));
}

TEST_CASE("seven coupons occupy exactly one rotating semicircle") {
    const auto coupons = score_tracker_segments(0, 0.0);
    REQUIRE(coupons.size() == kCouponCount); // Shape zero is a single line.
    for (int slot = 0; slot < kCouponCount; ++slot) {
        const auto& line = coupons[static_cast<std::size_t>(slot)];
        const double angle = 3.141592653589793 * slot / (kCouponCount - 1);
        CHECK((line.x0 + line.x1) / 2 ==
              doctest::Approx(logical_width() * 0.5 + 125 * std::sin(angle)));
        CHECK((line.y0 + line.y1) / 2 == doctest::Approx(kCouponTrackY - 18 * std::cos(angle)));
    }
    CHECK(coupons.front().depth == doctest::Approx(-1.f));
    CHECK(coupons.back().depth == doctest::Approx(1.f));
    CHECK((coupons.front().y0 + coupons.front().y1) / 2 == doctest::Approx(kCouponTrackY - 18.f));
    CHECK((coupons.back().y0 + coupons.back().y1) / 2 == doctest::Approx(kCouponTrackY + 18.f));
}

TEST_CASE("the carousel's highest coupon edge is one coupon height from the top") {
    float top = 1e9f;
    for (int step = 0; step < 240; ++step) {
        for (const auto& line : score_tracker_segments(116279, step / 240.0)) {
            top = std::min({top, line.y0, line.y1});
        }
    }
    CHECK(top >= kCouponHeightPx - 0.01f);
    CHECK(top == doctest::Approx(kCouponHeightPx).epsilon(0.1));
}

TEST_CASE("coupon rotation integrates stage scroll speed without transition jumps") {
    CourseTimeline slow;
    CourseEvent event;
    event.hit_ms = 4000;
    event.scroll_approach_ms = 2000;
    slow.events.push_back(event);
    auto fast = slow;
    fast.events.front().scroll_approach_ms = 1000;
    // One revolution per screen crossing: a 4000 ms crossing turns a quarter in 1000 ms.
    CHECK(score_tracker_phase(slow, 1000) == doctest::Approx(0.25));
    CHECK(score_tracker_phase(fast, 1000) == doctest::Approx(0.5));
    CHECK(score_tracker_phase(fast, 1000) == score_tracker_phase(slow, 2000));
    // The prelude turns at the first obstacle's speed and joins zero smoothly.
    CHECK(score_tracker_phase(slow, -1000) == doctest::Approx(-0.25));
    CHECK(score_tracker_phase(fast, -1000) == doctest::Approx(-0.5));
    CHECK(score_tracker_phase(slow, -8000) == doctest::Approx(-2.0));
    CHECK(score_tracker_phase(slow, -1) < score_tracker_phase(slow, 0));
    CHECK(score_tracker_phase(slow, 1) - score_tracker_phase(slow, 0) ==
          doctest::Approx(score_tracker_phase(slow, 0) - score_tracker_phase(slow, -1)));
    event.hit_ms = 8000;
    event.scroll_approach_ms = 1000;
    slow.events.push_back(event);
    CHECK(score_tracker_phase(slow, 3999) == doctest::Approx(3999.0 / 4000));
    CHECK(score_tracker_phase(slow, 4000) == doctest::Approx(1.0));
    CHECK(score_tracker_phase(slow, 4001) == doctest::Approx(1.0 + 1.0 / 2000));
    CHECK(score_tracker_phase(slow, 8000) == doctest::Approx(3.0));
    CHECK(score_tracker_phase(slow, 9000) == doctest::Approx(3.5));
    CHECK(score_tracker_phase(CourseTimeline{}, 1000) == doctest::Approx(0.25));
    CHECK(score_tracker_phase(CourseTimeline{}, -1000) == doctest::Approx(-0.25));
    slow.events.front().scroll_approach_ms = 0;
    slow.events.front().approach_ms = 2000;
    CHECK(score_tracker_phase(slow, 1000) == doctest::Approx(0.25));
}
