// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Charts, judgment, forms, and the built-in camera.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/attack.hpp"
#include "oscilline/course/beats.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/figure.hpp"
#include "oscilline/course/jitter.hpp"
#include "oscilline/course/load.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/course/shapes.hpp"
#include "oscilline/fsl.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/render/viewport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Words {
    std::vector<std::uint8_t> bytes;

    void u32(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
};

void header(Words& words,
            std::uint32_t control,
            std::uint32_t pattern,
            std::uint32_t event,
            std::uint32_t tracks,
            std::uint32_t fixed,
            std::uint32_t distribution) {
    words.u32(control);
    words.u32(pattern);
    words.u32(event);
    words.u32(tracks);
    words.u32(fixed);
    words.u32(distribution);
    words.u32(0);
    words.u32(0);
    words.u32(0);
}

struct Chart {
    struct Fixed {
        std::vector<std::uint32_t> obstacles;
    };
    struct Dist {
        std::uint32_t count = 1;
        std::int32_t seed = 0;
        std::int32_t max_prob = 1;
        std::int32_t prob[10] = {};
    };
    struct Control {
        std::int32_t start = 0;
        std::int32_t speed = 4410;
        std::int32_t shadow = 22050;
        std::int32_t delta_speed = 0;
        std::int32_t delta_shadow = 0;
    };
    struct Pattern {
        std::int32_t start = 0;
        std::int32_t type = 1;
        std::int32_t index = 0;
    };

    std::vector<Fixed> fixed;
    std::vector<Dist> distributions;
    std::vector<Control> control;
    std::vector<Pattern> patterns;
    // Track-index rows, all pointing at the one control and pattern track.
    // Course number is the 1-based index, so courses == 3 makes track 2 course 3.
    int courses = 1;
};

std::vector<std::uint8_t> chart_bytes(const Chart& chart) {
    Words words;
    header(words,
           1,
           1,
           1,
           static_cast<std::uint32_t>(chart.courses),
           static_cast<std::uint32_t>(chart.fixed.size()),
           static_cast<std::uint32_t>(chart.distributions.size()));
    for (const Chart::Fixed& pattern : chart.fixed) {
        words.u32(static_cast<std::uint32_t>(pattern.obstacles.size()));
        for (const std::uint32_t obstacle : pattern.obstacles) {
            words.u32(obstacle);
        }
    }
    for (const Chart::Dist& pattern : chart.distributions) {
        words.u32(pattern.count);
        words.i32(pattern.seed);
        words.i32(pattern.max_prob);
        for (const std::int32_t probability : pattern.prob) {
            words.i32(probability);
        }
    }
    words.u32(static_cast<std::uint32_t>(chart.control.size()));
    for (const Chart::Control& segment : chart.control) {
        words.i32(segment.start);
        words.i32(segment.speed);
        words.i32(segment.shadow);
        words.i32(segment.delta_speed);
        words.i32(segment.delta_shadow);
    }
    words.u32(static_cast<std::uint32_t>(chart.patterns.size()));
    for (const Chart::Pattern& segment : chart.patterns) {
        words.i32(segment.start);
        words.i32(0);
        words.i32(segment.type);
        words.i32(segment.index);
    }
    words.u32(0);
    for (int course = 0; course < chart.courses; ++course) {
        words.u32(0);
        words.u32(0);
        words.u32(0);
    }
    return words.bytes;
}

std::string type_letters(const oscilline::CourseTimeline& timeline) {
    std::string letters;
    letters.reserve(timeline.events.size());
    for (const oscilline::CourseEvent& event : timeline.events) {
        static constexpr char kSingle[] = {'B', 'P', 'L', 'W'};
        if (event.obstacle < 4) {
            letters.push_back(kSingle[event.obstacle]);
        } else {
            letters.push_back(static_cast<char>('0' + event.obstacle));
        }
    }
    return letters;
}

oscilline::FslFile must_fsl(const Chart& chart) {
    auto file = oscilline::parse_fsl(chart_bytes(chart));
    REQUIRE(file);
    return std::move(file.value());
}

oscilline::CourseTimeline must_course(const oscilline::FslFile& file,
                                      int track,
                                      std::int32_t audio_ms,
                                      oscilline::CourseMapOptions options = {}) {
    auto timeline = oscilline::build_course(file, track, audio_ms, options);
    REQUIRE(timeline);
    return std::move(timeline.value());
}

oscilline::CourseTimeline
chart_event(std::uint8_t obstacle, std::int32_t hit, std::int32_t duration) {
    oscilline::CourseTimeline timeline;
    timeline.duration_ms = duration;
    oscilline::CourseEvent event;
    event.obstacle = obstacle;
    event.hit_ms = hit;
    event.approach_ms = 512;
    timeline.events.push_back(event);
    return timeline;
}

std::int64_t perfect_at(const oscilline::CourseEvent& event) {
    const oscilline::ObstacleWindow window = oscilline::obstacle_window(event);
    return (window.perfect_open + window.perfect_close) / 2;
}

oscilline::CourseTimeline hits_at(int count, int gap, int duration) {
    oscilline::CourseTimeline timeline;
    timeline.duration_ms = duration;
    for (int i = 0; i < count; ++i) {
        oscilline::CourseEvent event;
        event.obstacle = 0;
        event.hit_ms = 1000 + i * gap;
        event.approach_ms = 400;
        timeline.events.push_back(event);
    }
    return timeline;
}

Chart::Dist seed_distribution(std::uint32_t count) {
    Chart::Dist pattern;
    pattern.count = count;
    pattern.seed = 0;
    pattern.max_prob = 6;
    const std::int32_t probs[10] = {0, 0, 1, 2, 2, 2, 2, 4, 6, 6};
    for (int i = 0; i < 10; ++i) {
        pattern.prob[i] = probs[i];
    }
    return pattern;
}

} // namespace

TEST_CASE("loop visual scale leaves block loop unchanged and opens the pit spike join") {
    using namespace oscilline;
    constexpr float kLoop = 100.f;
    CHECK(obstacle_width(kLoopObstacleId, kLoop) == doctest::Approx(kLoop * kLoopVisualScale));
    CHECK(obstacle_width(5, kLoop) ==
          doctest::Approx(kLoop * kArchWidthFraction * kArchHeightToWidth));

    const std::vector<Segment> shape = obstacle_shape(7, kLoop);
    const float radius = kLoop * kLoopVisualScale * kLoopRadiusFraction;
    const float cy = -kLoopRibbonLift * radius;
    const float cx = obstacle_leading_x(7, kLoop) + radius;
    const float top = std::numbers::pi_v<float> * 0.5f;
    for (const Segment& segment : shape) {
        const auto on_circle = [&](float x, float y) {
            return std::fabs(std::hypot(x - cx, y - cy) - radius) < 0.01f;
        };
        if (!on_circle(segment.x0, segment.y0) || !on_circle(segment.x1, segment.y1)) {
            continue;
        }
        const auto angle = [&](float x, float y) {
            float value = std::atan2(cy - y, x - cx);
            if (value < 0.f) {
                value += 2.f * std::numbers::pi_v<float>;
            }
            return value;
        };
        const float a = angle(segment.x0, segment.y0);
        const float b = angle(segment.x1, segment.y1);
        const float left_join = top + 0.58f;
        const float right_join = top - 0.58f;
        const bool both_inside_spike_gap =
            (a >= right_join && a <= left_join) && (b >= right_join && b <= left_join);
        CHECK_FALSE(both_inside_spike_gap);
    }
}

TEST_CASE("obstacle ids map to the four buttons and their pairs") {
    using namespace oscilline;
    CHECK(obstacle_actions(0) == kActionBlock);
    CHECK(obstacle_actions(1) == kActionPit);
    CHECK(obstacle_actions(2) == kActionLoop);
    CHECK(obstacle_actions(3) == kActionWave);
    CHECK(obstacle_actions(4) == (kActionBlock | kActionPit));
    CHECK(obstacle_actions(5) == (kActionBlock | kActionLoop));
    CHECK(obstacle_actions(6) == (kActionBlock | kActionWave));
    CHECK(obstacle_actions(7) == (kActionPit | kActionLoop));
    CHECK(obstacle_actions(8) == (kActionPit | kActionWave));
    CHECK(obstacle_actions(9) == (kActionLoop | kActionWave));
    CHECK(obstacle_actions(10) == 0);
    CHECK(obstacle_name(0) == "block");
    CHECK(obstacle_name(9) == "loop+wave");
}

TEST_CASE("distribution buckets are cumulative") {
    const std::int32_t probs[10] = {0, 0, 1, 2, 2, 2, 2, 4, 6, 6};
    CHECK(oscilline::distribution_index(probs, 0) == 2);
    CHECK(oscilline::distribution_index(probs, 1) == 3);
    CHECK(oscilline::distribution_index(probs, 3) == 7);
    CHECK(oscilline::distribution_index(probs, 5) == 8);
    CHECK(oscilline::distribution_index(probs, 6) == -1);
}

TEST_CASE("fixed pattern repeats on the shadow period") {
    Chart chart;
    chart.fixed.push_back({{0, 2, 3}});
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 3, 1, 0});
    chart.patterns.push_back({22050 * 6, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    CHECK(timeline.cdda_track == 2);
    REQUIRE(timeline.events.size() == 3);
    CHECK(timeline.events[0].obstacle == 0);
    CHECK(timeline.events[0].hit_ms == 3000);
    CHECK(timeline.events[0].approach_ms == 1000);
    CHECK(timeline.events[1].obstacle == 2);
    CHECK(timeline.events[1].hit_ms == 4000);
    CHECK(timeline.events[2].obstacle == 3);
    CHECK(timeline.events[2].hit_ms == 5000);

    oscilline::CourseMapOptions options;
    options.units_per_second = 44100.0;
    const auto scaled = must_course(must_fsl(chart), 0, 0, options);
    // At this time base the speed is shorter than the shortest beat, so the
    // grid uses kMinBeatMs and the one-second shadow rounds to four beats.
    REQUIRE(scaled.events.size() == 3);
    CHECK(scaled.events[1].hit_ms == 2100);
    CHECK(scaled.events[1].approach_ms == 1000);
}

TEST_CASE("shadow delta spaces later obstacles further apart") {
    Chart chart;
    chart.fixed.push_back({{0, 0, 0}});
    Chart::Control control;
    control.start = 22050 * 2;
    control.shadow = 22050;
    control.delta_shadow = 4096;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 6, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 3);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[1].hit_ms == 3000);
    CHECK(timeline.events[2].hit_ms == 5000);
}

TEST_CASE("a negative shadow delta never undercuts the base gap") {
    Chart chart;
    chart.fixed.push_back({{1}});
    Chart::Control control;
    control.shadow = 22050;
    control.delta_shadow = -409600;
    chart.control.push_back(control);
    chart.patterns.push_back({0, 1, 0});
    chart.patterns.push_back({22050 * 60, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 58);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[57].hit_ms == 59000);
    CHECK(timeline.events.size() < static_cast<std::size_t>(oscilline::kMaxCourseEvents));
}

TEST_CASE("a one-obstacle pattern loops until the next segment") {
    Chart chart;
    chart.fixed.push_back({{1}});
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 5, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 3);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[1].hit_ms == 3000);
    CHECK(timeline.events[2].hit_ms == 4000);
}

TEST_CASE("each pattern type obeys its record") {
    Chart chart;
    chart.fixed.push_back({{0, 2}});
    chart.distributions.push_back(seed_distribution(2));
    chart.control.push_back({});
    chart.patterns.push_back({0, 0, 0});
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 5, 2, 0});
    chart.patterns.push_back({22050 * 7, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 5);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[0].obstacle == 0);
    CHECK(timeline.events[1].hit_ms == 3000);
    CHECK(timeline.events[1].obstacle == 2);
    CHECK(timeline.events[2].hit_ms == 4000);
    CHECK(timeline.events[2].obstacle == 0);
    CHECK(timeline.events[3].hit_ms == 5000);
    // Fixed obstacles take no step, so the random section uses rolls 1 and 2.
    CHECK(timeline.events[3].obstacle == 3);
    CHECK(timeline.events[4].hit_ms == 6000);
    CHECK(timeline.events[4].obstacle == 3);
}

TEST_CASE("distribution count does not limit the obstacles") {
    Chart chart;
    chart.distributions.push_back(seed_distribution(1));
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 2, 2, 0});
    chart.patterns.push_back({22050 * 8, 0, 0});

    const auto once = must_course(must_fsl(chart), 0, 0);
    REQUIRE(once.events.size() == 6);
    CHECK(once.events[0].obstacle == 3);
    CHECK(once.events[1].obstacle == 3);
    CHECK(once.events[2].obstacle == 8);
    CHECK(once.events[3].obstacle == 8);
    CHECK(once.events[4].obstacle == 7);
    CHECK(once.events[5].obstacle == 7);

    chart.distributions[0].count = 0;
    const auto zero = must_course(must_fsl(chart), 0, 0);
    REQUIRE(zero.events.size() == once.events.size());
    CHECK(zero.events[0].obstacle == once.events[0].obstacle);
    CHECK(zero.events[5].obstacle == once.events[5].obstacle);
}

Chart::Dist loop_wave_distribution() {
    Chart::Dist pattern;
    pattern.seed = 11;
    pattern.max_prob = 3;
    // Two loops for each wave. The first 17 high-bit rolls from seed 11 are
    // the bronze course 1 prefix below.
    const std::int32_t probs[10] = {0, 0, 2, 3, 3, 3, 3, 3, 3, 3};
    for (int i = 0; i < 10; ++i) {
        pattern.prob[i] = probs[i];
    }
    return pattern;
}

Chart::Dist block_wave_distribution() {
    Chart::Dist pattern;
    pattern.seed = 311;
    pattern.max_prob = 2;
    const std::int32_t probs[10] = {1, 1, 1, 2, 2, 2, 2, 2, 2, 2};
    for (int i = 0; i < 10; ++i) {
        pattern.prob[i] = probs[i];
    }
    return pattern;
}

TEST_CASE("bronze course 1 random prefix ignores the two lead-in cuts") {
    Chart dropped;
    dropped.distributions.push_back(loop_wave_distribution());
    dropped.control.push_back({});
    dropped.patterns.push_back({0, 2, 0});
    dropped.patterns.push_back({22050 * 19, 0, 0});

    const auto timeline = must_course(must_fsl(dropped), 0, 0);
    REQUIRE(timeline.events.size() == 17);
    CHECK(timeline.events.front().hit_ms == 2000);
    CHECK(type_letters(timeline) == "LLLLWWLWLLWLLLLLL");

    // Same 17 rolls when the section starts late enough that nothing is cut.
    Chart kept;
    kept.distributions.push_back(loop_wave_distribution());
    kept.control.push_back({});
    kept.patterns.push_back({22050 * 2, 2, 0});
    kept.patterns.push_back({22050 * 19, 0, 0});
    const auto late = must_course(must_fsl(kept), 0, 0);
    REQUIRE(late.events.size() == 17);
    CHECK(type_letters(late) == type_letters(timeline));
}

TEST_CASE("silver course 3 random prefix uses seed times course number") {
    Chart chart;
    chart.courses = 3;
    chart.distributions.push_back(block_wave_distribution());
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 2, 2, 0});
    chart.patterns.push_back({22050 * 22, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 2, 0);
    REQUIRE(timeline.events.size() == 20);
    CHECK(type_letters(timeline) == "WWWBBBWBBBBWBWBBWBWB");

    // 2 * tier + 1 is 1 for both bronze courses, so course 2 must not reuse it.
    Chart bronze;
    bronze.courses = 2;
    Chart::Dist half = block_wave_distribution();
    half.seed = 11;
    bronze.distributions.push_back(half);
    bronze.control.push_back({});
    bronze.patterns.push_back({22050 * 2, 2, 0});
    bronze.patterns.push_back({22050 * 10, 0, 0});
    const auto file = must_fsl(bronze);
    const auto course1 = must_course(file, 0, 0);
    const auto course2 = must_course(file, 1, 0);
    REQUIRE(course1.events.size() == course2.events.size());
    REQUIRE(course1.events.size() >= 8);
    CHECK(type_letters(course1).substr(0, 8) == "BBBBWWBW");
    CHECK(type_letters(course2).substr(0, 8) == "BBWBBBWW");
}

TEST_CASE("fixed obstacles take no step and a new record reseeds") {
    Chart chart;
    chart.fixed.push_back({{4}});
    chart.distributions.push_back(seed_distribution(1));
    Chart::Dist later = seed_distribution(1);
    later.seed = 1;
    chart.distributions.push_back(later);
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 2, 2, 0});
    chart.patterns.push_back({22050 * 4, 1, 0});
    chart.patterns.push_back({22050 * 6, 2, 1});
    chart.patterns.push_back({22050 * 8, 2, 1});
    chart.patterns.push_back({22050 * 10, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 8);
    CHECK(type_letters(timeline) == "WW44W778");
}

TEST_CASE("times and pair flags match the low-bit picker") {
    Chart chart;
    chart.fixed.push_back({{4, 0, 9}});
    Chart::Dist half = block_wave_distribution();
    half.seed = 11;
    chart.distributions.push_back(half);
    chart.control.push_back({});
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 5, 2, 0});
    chart.patterns.push_back({22050 * 8, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 6);
    // Previous picker, same chart: fixed 4, 0, 9 then three singles. The
    // random ids were B W B from the low bits; they are B B B now.
    const std::pair<std::int32_t, bool> old_output[] = {
        {2000, true},
        {3000, false},
        {4000, true},
        {5000, false},
        {6000, false},
        {7000, false},
    };
    for (std::size_t i = 0; i < timeline.events.size(); ++i) {
        CHECK(timeline.events[i].hit_ms == old_output[i].first);
        CHECK(oscilline::obstacle_is_pair(timeline.events[i].obstacle) == old_output[i].second);
    }
    CHECK(type_letters(timeline).substr(3) == "BBB");
}

TEST_CASE("random sections repeat near the i.i.d. rate and do not alternate") {
    Chart chart;
    Chart::Control control;
    control.speed = 4410;
    control.shadow = 4410;
    chart.control.push_back(control);
    Chart::Dist half = block_wave_distribution();
    half.seed = 11;
    chart.distributions.push_back(half);
    chart.distributions.push_back(loop_wave_distribution());
    const std::int32_t beat = 4410;
    const int count = 120;
    const std::int32_t start = 22050 * 2;
    chart.patterns.push_back({start, 2, 0});
    chart.patterns.push_back({start + count * beat, 2, 1});
    chart.patterns.push_back({start + 2 * count * beat, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == static_cast<std::size_t>(2 * count));

    const auto share =
        [](const oscilline::CourseTimeline& course, std::size_t begin, std::size_t end) {
            int repeats = 0;
            int compared = 0;
            bool alternating = true;
            for (std::size_t i = begin + 1; i < end; ++i) {
                ++compared;
                if (course.events[i].obstacle == course.events[i - 1].obstacle) {
                    ++repeats;
                    alternating = false;
                }
            }
            return std::pair<double, bool>{
                compared == 0 ? 0.0 : static_cast<double>(repeats) / compared, alternating};
        };
    const auto [half_share, half_alternating] = share(timeline, 0, static_cast<std::size_t>(count));
    const auto [biased_share, biased_alternating] =
        share(timeline, static_cast<std::size_t>(count), timeline.events.size());
    CHECK_FALSE(half_alternating);
    CHECK_FALSE(biased_alternating);
    CHECK(std::abs(half_share - 0.5) < 0.15);
    CHECK(std::abs(biased_share - (5.0 / 9.0)) < 0.15);
    const double pooled = (half_share + biased_share) / 2.0;
    const double expected = (0.5 + 5.0 / 9.0) / 2.0;
    CHECK(std::abs(pooled - expected) < 0.15);
}

TEST_CASE("a tiny shadow means one beat") {
    Chart chart;
    chart.fixed.push_back({{1}});
    Chart::Control control;
    control.shadow = 1;
    chart.control.push_back(control);
    chart.patterns.push_back({0, 1, 0});
    chart.patterns.push_back({22050 * 30, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 142);
    CHECK(timeline.events[0].hit_ms == 1600);
    CHECK(timeline.events[1].hit_ms == 1800);
    CHECK(timeline.events.size() < static_cast<std::size_t>(oscilline::kMaxCourseEvents));
}

TEST_CASE("a shadow just under a whole number of beats rounds to it") {
    Chart chart;
    chart.fixed.push_back({{0}});
    Chart::Control control;
    control.shadow = 4410 * 2 - 200;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 4, 0, 0});

    const auto near = must_course(must_fsl(chart), 0, 0);
    REQUIRE(near.events.size() == 5);
    CHECK(near.events[1].hit_ms - near.events[0].hit_ms == 400);

    chart.control[0].shadow = 4410 * 2 + 600;
    const auto over = must_course(must_fsl(chart), 0, 0);
    REQUIRE(over.events.size() == 4);
    CHECK(over.events[1].hit_ms - over.events[0].hit_ms == 600);
}

TEST_CASE("beats from the music replace the speed grid") {
    Chart chart;
    chart.fixed.push_back({{0, 1}});
    Chart::Control control;
    control.shadow = 4410 * 2;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 4, 0, 0});

    // Beats near the 200 ms speed period, slightly uneven, with a stray one.
    const std::vector<std::int32_t> beats = {
        1990, 2190, 2250, 2395, 2610, 2800, 3010, 3190, 3420, 3600, 3810, 4100};
    oscilline::CourseMapOptions options;
    options.beat_ms = beats;
    const auto timeline = must_course(must_fsl(chart), 0, 0, options);
    REQUIRE(timeline.events.size() == 5);
    CHECK(timeline.events[0].hit_ms == 2190);
    CHECK(timeline.events[1].hit_ms == 2610);
    CHECK(timeline.events[2].hit_ms == 3010);
    CHECK(timeline.events[3].hit_ms == 3420);
    CHECK(timeline.events[4].hit_ms == 3810);
    CHECK(timeline.events[1].obstacle == 1);
}

TEST_CASE("attack emphasis is the short over the long window power of the mono mix") {
    // Silence gives zeros, a tone after silence gives one, and a steady tone
    // settles at 1024 frames over 8192.
    const int hop = oscilline::kAttackHopFrames;
    const int frames = hop * 160;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * 2, 0);
    for (int i = 80 * hop; i < frames; ++i) {
        pcm[static_cast<std::size_t>(i) * 2] =
            static_cast<std::int16_t>((i % 2) == 0 ? 8000 : -8000);
    }
    const auto ratio = oscilline::attack_emphasis(pcm, frames);
    REQUIRE(ratio.size() == 160);
    CHECK(ratio[10] == doctest::Approx(0.0));
    CHECK(ratio[80] == doctest::Approx(1.0));
    CHECK(ratio[159] == doctest::Approx(1024.0 / 8192.0));

    // Opposite channels cancel in the mix.
    for (int i = 0; i < frames; ++i) {
        pcm[static_cast<std::size_t>(i) * 2 + 1] =
            static_cast<std::int16_t>(-pcm[static_cast<std::size_t>(i) * 2]);
    }
    for (const float value : oscilline::attack_emphasis(pcm, frames)) {
        CHECK(value == 0.0f);
    }
    // Nothing is offered before the long window is full, even for a loud start.
    std::vector<std::int16_t> loud(static_cast<std::size_t>(frames) * 2, 0);
    for (int i = 0; i < frames; ++i) {
        loud[static_cast<std::size_t>(i) * 2] =
            static_cast<std::int16_t>((i % 2) == 0 ? 8000 : -8000);
    }
    const auto opening = oscilline::attack_emphasis(loud, frames);
    const std::size_t full = oscilline::kAttackLongFrames / hop - 1;
    CHECK(opening[0] == 0.0f);
    CHECK(opening[full - 1] == 0.0f);
    CHECK(opening[full] == doctest::Approx(1024.0 / 8192.0));
    CHECK(oscilline::attack_stamp_frame(343) == 44152);
    CHECK(oscilline::attack_stamp_ms(343) == doctest::Approx(44152.0 / 44.1));
    CHECK(oscilline::attack_emphasis({}, 0).empty());

    // A noise floor still has a ratio. It is silence, and the chart ends at
    // the last loud hop rather than the file length.
    constexpr int kToneFrames = oscilline::kCddaRate;
    constexpr int kTailFrames = oscilline::kCddaRate * 3;
    const int mixed = kToneFrames + kTailFrames;
    std::vector<std::int16_t> tail(static_cast<std::size_t>(mixed) * 2, 8);
    for (int i = 0; i < kToneFrames; ++i) {
        tail[static_cast<std::size_t>(i) * 2] =
            static_cast<std::int16_t>((i % 2) == 0 ? 8000 : -8000);
        tail[static_cast<std::size_t>(i) * 2 + 1] = tail[static_cast<std::size_t>(i) * 2];
    }
    const auto mixed_ratio = oscilline::attack_emphasis(tail, mixed);
    REQUIRE(!mixed_ratio.empty());
    const std::size_t loud_hop = static_cast<std::size_t>(oscilline::kAttackLongFrames / hop);
    CHECK(mixed_ratio[loud_hop] == doctest::Approx(1024.0 / 8192.0));
    const std::size_t quiet_hop =
        static_cast<std::size_t>((kToneFrames + oscilline::kCddaRate) / hop);
    REQUIRE(quiet_hop < mixed_ratio.size());
    CHECK(mixed_ratio[quiet_hop] == 0.0f);
    CHECK(mixed_ratio.back() == 0.0f);
    const std::int32_t file_ms = oscilline::cdda_duration_ms(mixed);
    const std::int32_t audible = oscilline::attack_audible_end_ms(tail, mixed);
    CHECK(audible > 1000);
    CHECK(audible < file_ms);
    CHECK(oscilline::attack_audible_end_ms(loud, frames) == oscilline::cdda_duration_ms(frames));
    std::vector<std::int16_t> floor_only(static_cast<std::size_t>(frames) * 2, 8);
    CHECK(oscilline::attack_audible_end_ms(floor_only, frames) == 0);
    for (const float value : oscilline::attack_emphasis(floor_only, frames)) {
        CHECK(value == 0.0f);
    }
}

namespace {

// First evaluation stamped at or after `ms`.
std::size_t stamp_index(double ms) {
    std::size_t k = 0;
    while (oscilline::attack_stamp_ms(k) < ms) {
        ++k;
    }
    return k;
}

double stamp_at(double ms) {
    return oscilline::attack_stamp_ms(stamp_index(ms));
}

// Speed 200 ms, so an attack commits 400 ms later. Shadow 200 ms, so the
// select windows are 200 ms long and end on multiples of 200 ms.
std::vector<float> attack_fixture() {
    std::vector<float> emphasis(stamp_index(6000.0), 0.0f);
    const auto put = [&](double ms, float value) { emphasis[stamp_index(ms)] = value; };
    put(3000.0, 0.4f);  // pending at 3200, committed at 3400
    put(3050.0, 0.35f); // same window, smaller: not a potential
    put(3250.0, 0.5f);  // under 400 ms after 3000: dropped
    put(3500.0, 0.3f);  // pending at 3600
    put(3700.0, 0.5f);  // before 3900 and over 1.5 times 0.3: replaces 3500
    put(3850.0, 0.7f);  // before 4100 but not over 1.5 times 0.5: dropped
    put(4600.0, 0.2f);  // committed at the end
    return emphasis;
}

oscilline::FslFile attack_chart(std::int32_t delta, bool with_break) {
    Chart chart;
    chart.fixed.push_back({{0, 1}});
    Chart::Control control;
    control.speed = 4410;
    control.shadow = 4410;
    // The last control segment holds its base shadow, whatever its ramp.
    control.delta_shadow = delta;
    chart.control.push_back(control);
    chart.patterns.push_back({0, 1, 0});
    if (with_break) {
        chart.patterns.push_back({70560, 0, 0});     // break from 3.2 s
        chart.patterns.push_back({22050 * 4, 1, 0}); // back at 4 s
    }
    return must_fsl(chart);
}

} // namespace

TEST_CASE("attacks commit two speeds after the pending attack and replace on 1.5 times") {
    for (const std::int32_t delta : {0, 1 << 22}) {
        CAPTURE(delta);
        const auto file = attack_chart(delta, false);
        const std::vector<float> emphasis = attack_fixture();

        const auto attacks = oscilline::course_attack_ms(file, 0, emphasis);
        REQUIRE(attacks);
        REQUIRE(attacks.value().size() == 3);
        CHECK(attacks.value()[0] == doctest::Approx(stamp_at(3000.0)));
        CHECK(attacks.value()[1] == doctest::Approx(stamp_at(3700.0)));
        CHECK(attacks.value()[2] == doctest::Approx(stamp_at(4600.0)));

        oscilline::CourseMapOptions options;
        options.emphasis = emphasis;
        // Beats are ignored once emphasis is present.
        const std::vector<std::int32_t> beats = {2000, 2500, 3000};
        options.beat_ms = beats;
        const auto timeline = must_course(file, 0, 0, options);
        REQUIRE(timeline.events.size() == 3);
        // Each obstacle collides at its attack.
        CHECK(timeline.events[0].hit_ms == std::lround(stamp_at(3000.0)));
        CHECK(timeline.events[1].hit_ms == std::lround(stamp_at(3700.0)));
        CHECK(timeline.events[2].hit_ms == std::lround(stamp_at(4600.0)));
        CHECK(timeline.events[0].obstacle == 0);
        CHECK(timeline.events[1].obstacle == 1);
    }
}

TEST_CASE("a pending attack is not committed after the music ends") {
    using namespace oscilline;
    const auto file = attack_chart(0, false);
    const std::vector<float> emphasis = attack_fixture();
    const auto open = course_attack_ms(file, 0, emphasis);
    REQUIRE(open);
    REQUIRE(open.value().size() == 3);

    const double late = stamp_at(4600.0);
    const auto cut_ms = static_cast<std::int32_t>(std::lround(late + 100.0));
    const auto cut = course_attack_ms(file, 0, emphasis, {}, cut_ms);
    REQUIRE(cut);
    REQUIRE(cut.value().size() == 2);
    CHECK(cut.value()[0] == doctest::Approx(stamp_at(3000.0)));
    CHECK(cut.value()[1] == doctest::Approx(stamp_at(3700.0)));

    CourseMapOptions options;
    options.emphasis = emphasis;
    const auto trimmed = must_course(file, 0, cut_ms, options);
    REQUIRE(trimmed.events.size() == 2);
    CHECK(trimmed.audio_end_ms == cut_ms);
    for (const CourseEvent& event : trimmed.events) {
        CHECK(event.hit_ms <= trimmed.audio_end_ms);
        CHECK_FALSE(obstacle_after_audio(event.hit_ms, event.approach_ms, trimmed.audio_end_ms));
    }
    PlayState at_end;
    play_advance(at_end, trimmed, 0, trimmed.audio_end_ms, 0);
    PlayState past;
    play_advance(past, trimmed, 0, static_cast<std::int64_t>(trimmed.audio_end_ms) + 2000, 0);
    CHECK(past.misses == at_end.misses);
    CHECK(past.perfects == at_end.perfects);
    CHECK(past.goods == at_end.goods);
    CHECK(past.score == at_end.score);
    CHECK(past.misses == 2);
}

TEST_CASE("a noise floor after a tone is not charted") {
    using namespace oscilline;
    constexpr int kToneFrames = kCddaRate;
    constexpr int kTailFrames = kCddaRate * 3;
    const int mixed = kToneFrames + kTailFrames;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(mixed) * 2, 8);
    for (int i = 0; i < kToneFrames; ++i) {
        pcm[static_cast<std::size_t>(i) * 2] =
            static_cast<std::int16_t>((i % 2) == 0 ? 8000 : -8000);
        pcm[static_cast<std::size_t>(i) * 2 + 1] = pcm[static_cast<std::size_t>(i) * 2];
    }
    const std::vector<float> emphasis = attack_emphasis(pcm, mixed);
    const std::int32_t audible = attack_audible_end_ms(pcm, mixed);
    const std::int32_t file_ms = cdda_duration_ms(mixed);
    REQUIRE(audible > 1000);
    REQUIRE(audible < file_ms);

    CourseMapOptions options;
    options.emphasis = emphasis;
    const auto file = attack_chart(0, false);
    const auto timeline = must_course(file, 0, audible, options);
    CHECK(!timeline.events.empty());
    for (const CourseEvent& event : timeline.events) {
        CHECK(event.hit_ms <= timeline.audio_end_ms);
        const std::int32_t travel = stage_scroll_approach_ms(event);
        CHECK_FALSE(obstacle_after_audio(event.hit_ms, travel, timeline.audio_end_ms));
    }
    const auto full = must_course(file, 0, file_ms, options);
    for (const CourseEvent& event : full.events) {
        CHECK(event.hit_ms < file_ms - 2000);
    }
    PlayState at_end;
    play_advance(at_end, timeline, 0, timeline.audio_end_ms, 0);
    PlayState past;
    play_advance(past, timeline, 0, static_cast<std::int64_t>(timeline.audio_end_ms) + 2000, 0);
    CHECK(past.misses == at_end.misses);
    CHECK(past.perfects == at_end.perfects);
    CHECK(past.score == at_end.score);
}

TEST_CASE("missing audio does not fill the ribbon from the beat grid") {
    Chart chart;
    chart.distributions.push_back(seed_distribution(1));
    chart.control.push_back({});
    chart.patterns.push_back({0, 2, 0});
    const auto grid = must_course(must_fsl(chart), 0, 20000);
    CHECK(grid.events.size() > 10);

    const std::vector<float> none{0.f};
    oscilline::CourseMapOptions silent;
    silent.emphasis = none;
    const auto empty = must_course(must_fsl(chart), 0, 20000, silent);
    CHECK(empty.events.empty());
}

TEST_CASE("an early attack scrolls at the segment speed") {
    const auto file = attack_chart(0, false);
    std::vector<float> emphasis(stamp_index(3000.0), 0.0f);
    emphasis[stamp_index(300.0)] = 0.5f;
    emphasis[stamp_index(2500.0)] = 0.5f;
    oscilline::CourseMapOptions options;
    options.emphasis = emphasis;
    const auto timeline = must_course(file, 0, 0, options);
    REQUIRE(timeline.events.size() == 2);
    const oscilline::CourseEvent& early = timeline.events[0];
    const oscilline::CourseEvent& later = timeline.events[1];
    CHECK(early.hit_ms == std::lround(stamp_at(300.0)));
    CHECK(early.obstacle == 0);
    CHECK(later.obstacle == 1);
    // 200 ms beat, four beats, clamped to the one-second floor. Both obstacles.
    CHECK(early.approach_ms == oscilline::kMinApproachMs);
    CHECK(early.scroll_approach_ms == later.scroll_approach_ms);
    CHECK(early.scroll_approach_ms == oscilline::kMinApproachMs);
    CHECK(oscilline::stage_scroll_approach_ms(early) == early.scroll_approach_ms * 2);
    CHECK(early.hit_ms < early.scroll_approach_ms);

    constexpr float kWidth = 640.f;
    const float hit_x = kWidth * oscilline::kHitXFraction;
    const float spawn_x = kWidth * oscilline::kSpawnXFraction;
    const auto step = [&](const oscilline::CourseEvent& event) {
        const float before = oscilline::obstacle_screen_x(
            hit_x, spawn_x, oscilline::stage_scroll_approach_ms(event), event.hit_ms, 0);
        const float after = oscilline::obstacle_screen_x(
            hit_x, spawn_x, oscilline::stage_scroll_approach_ms(event), event.hit_ms, 100);
        return before - after;
    };
    CHECK(step(early) == doctest::Approx(step(later)));
    const float at_start = oscilline::obstacle_screen_x(
        hit_x, spawn_x, oscilline::stage_scroll_approach_ms(early), early.hit_ms, 0);
    CHECK(at_start > hit_x);
    CHECK(at_start < spawn_x);
}

TEST_CASE("an evaluation stamped on a window end belongs to the next window") {
    // Shadow 4410 units: windows end on multiples of 200 ms. Evaluation 1514 is
    // stamped at 97020 units (4400 ms), exactly on a window end. With a speed of
    // one unit, 1513 commits at once and 1514 is the next attack; were 1514 in
    // 1513's window, the larger 1513 would hide it.
    Chart chart;
    chart.fixed.push_back({{0, 1}});
    chart.control.push_back(Chart::Control{0, 1, 4410, 0, 0});
    chart.patterns.push_back({0, 1, 0});
    const auto file = must_fsl(chart);
    REQUIRE(oscilline::attack_stamp_frame(1514) == 97020 * 2);
    std::vector<float> emphasis(1600, 0.0f);
    emphasis[1513] = 0.9f;
    emphasis[1514] = 0.5f;
    const auto attacks = oscilline::course_attack_ms(file, 0, emphasis);
    REQUIRE(attacks);
    REQUIRE(attacks.value().size() == 2);
    CHECK(attacks.value()[0] == doctest::Approx(oscilline::attack_stamp_ms(1513)));
    CHECK(attacks.value()[1] == doctest::Approx(4400.0));
}

TEST_CASE("silent audio charts no attacks and a break offers no potentials") {
    const auto file = attack_chart(0, true);

    oscilline::CourseMapOptions options;
    const std::vector<float> silent(stamp_index(6000.0), 0.0f);
    options.emphasis = silent;
    CHECK(must_course(file, 0, 0, options).events.empty());

    // The break from 3.2 to 4 s hides 3250, 3500, 3700 and 3850, so 3000 is
    // followed by 4600.
    const std::vector<float> emphasis = attack_fixture();
    options.emphasis = emphasis;
    const auto timeline = must_course(file, 0, 0, options);
    REQUIRE(timeline.events.size() == 2);
    CHECK(timeline.events[0].hit_ms == std::lround(stamp_at(3000.0)));
    CHECK(timeline.events[1].hit_ms == std::lround(stamp_at(4600.0)));
}

TEST_CASE("dense beats are held at the minimum gap") {
    Chart chart;
    chart.fixed.push_back({{1}});
    Chart::Control control;
    control.speed = 0x40;
    control.shadow = 1;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 3, 0, 0});

    std::vector<std::int32_t> beats;
    for (std::int32_t ms = 2000; ms < 3000; ms += 50) {
        beats.push_back(ms);
    }
    oscilline::CourseMapOptions options;
    options.beat_ms = beats;
    const auto timeline = must_course(must_fsl(chart), 0, 0, options);
    REQUIRE(timeline.events.size() == 7);
    for (std::size_t i = 1; i < timeline.events.size(); ++i) {
        CHECK(timeline.events[i].hit_ms - timeline.events[i - 1].hit_ms == oscilline::kMinGapMs);
    }
}

TEST_CASE("spawn order follows hit order when the speed changes") {
    Chart chart;
    chart.fixed.push_back({{0, 1, 2, 3}});
    Chart::Control slow;
    slow.speed = 22050 * 7 / 10;
    slow.shadow = 1;
    chart.control.push_back(slow);
    Chart::Control fast;
    fast.start = 22050 * 8;
    fast.speed = 22050 / 5;
    fast.shadow = 1;
    chart.control.push_back(fast);
    chart.patterns.push_back({22050 * 3, 1, 0});
    chart.patterns.push_back({22050 * 14, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() > 20);
    bool slow_seen = false;
    bool fast_seen = false;
    for (std::size_t i = 0; i < timeline.events.size(); ++i) {
        const auto& event = timeline.events[i];
        CHECK(event.approach_ms > 0);
        CHECK(event.approach_ms <= oscilline::kMaxApproachMs);
        slow_seen = slow_seen || event.approach_ms > 2500;
        fast_seen = fast_seen || event.approach_ms < 1100;
        if (i > 0) {
            const auto& prev = timeline.events[i - 1];
            CHECK(prev.hit_ms - prev.approach_ms <= event.hit_ms - event.approach_ms);
        }
    }
    CHECK(slow_seen);
    CHECK(fast_seen);
}

TEST_CASE("onsets of a click track give its beats") {
    constexpr int kRate = 44100;
    constexpr int kFrames = kRate * 8;
    constexpr int kBeatMs = 500;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(kFrames) * 2, 0);
    for (int start = kRate / 4; start + 2000 < kFrames; start += kRate * kBeatMs / 1000) {
        for (int i = 0; i < 2000; ++i) {
            const double fade = 1.0 - i / 2000.0;
            const double value =
                12000.0 * fade * std::sin(i * 0.3) + 6000.0 * fade * std::sin(i * 1.1);
            const auto sample = static_cast<std::int16_t>(value);
            pcm[static_cast<std::size_t>(start + i) * 2] = sample;
            pcm[static_cast<std::size_t>(start + i) * 2 + 1] = sample;
        }
    }
    const auto envelope = oscilline::onset_envelope(pcm, kFrames);
    REQUIRE(envelope.size() > 700);

    // Expect a slightly wrong period; the tracker follows the clicks anyway.
    const std::vector<float> period(envelope.size(), 470.0f);
    const auto beats = oscilline::track_beats(envelope, period);
    REQUIRE(beats.size() >= 14);
    int on_click = 0;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        const int offset = (beats[i] - 250) % kBeatMs;
        if (offset <= 30 || offset >= kBeatMs - 30) {
            ++on_click;
        }
        if (i > 0) {
            CHECK(beats[i] - beats[i - 1] >= oscilline::kMinBeatMs);
        }
    }
    CHECK(on_click >= static_cast<int>(beats.size()) - 1);

    const std::vector<std::int16_t> silence(static_cast<std::size_t>(kFrames) * 2, 0);
    CHECK(oscilline::onset_envelope(silence, kFrames).empty());
    CHECK(oscilline::track_beats({}, {}).empty());
}

TEST_CASE("the beat period follows the speed value") {
    Chart chart;
    chart.fixed.push_back({{0}});
    Chart::Control control;
    control.speed = 11025;
    chart.control.push_back(control);
    chart.patterns.push_back({0, 1, 0});
    chart.patterns.push_back({22050 * 4, 0, 0});
    const auto file = must_fsl(chart);
    auto period = oscilline::course_beat_period_ms(file, 0, 1000);
    REQUIRE(period);
    CHECK(period.value() == doctest::Approx(500.0));
    chart.control[0].speed = 0x40;
    period = oscilline::course_beat_period_ms(must_fsl(chart), 0, 1000);
    REQUIRE(period);
    CHECK(period.value() == doctest::Approx(oscilline::kMinBeatMs));
    CHECK_FALSE(oscilline::course_beat_period_ms(file, 3, 0));
}

TEST_CASE("shadow spacing carries across pattern segments") {
    Chart chart;
    chart.fixed.push_back({{1}});
    chart.control.push_back({});
    const std::int32_t boundary = 22050 * 2 + 2359;
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({boundary, 1, 0});
    chart.patterns.push_back({22050 * 6, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() >= 2);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[1].hit_ms == 2907);
}

TEST_CASE("hits before the approach and lead-in are dropped") {
    Chart chart;
    chart.fixed.push_back({{4}});
    chart.control.push_back({});
    chart.patterns.push_back({0, 1, 0});
    chart.patterns.push_back({22050 * 4, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 2);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[1].hit_ms == 3000);
    CHECK(timeline.events[0].obstacle == 4);
    CHECK(timeline.events[0].hit_ms >= timeline.events[0].approach_ms + oscilline::kLeadInMs);
}

TEST_CASE("approach time is four beats, clamped to one to three seconds") {
    Chart chart;
    chart.fixed.push_back({{0}});
    Chart::Control control;
    control.shadow = 22050;
    control.speed = 4410;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 4, 1, 0});
    chart.patterns.push_back({22050 * 5, 0, 0});

    const auto fast = must_course(must_fsl(chart), 0, 0);
    REQUIRE(fast.events.size() == 1);
    CHECK(fast.events[0].approach_ms == oscilline::kMinApproachMs);

    chart.control[0].speed = 0x40;
    const auto crash_floor = must_course(must_fsl(chart), 0, 0);
    REQUIRE(crash_floor.events.size() == 1);
    CHECK(crash_floor.events[0].approach_ms == oscilline::kMinApproachMs);

    chart.control[0].speed = 13891;
    const auto mid = must_course(must_fsl(chart), 0, 0);
    REQUIRE(mid.events.size() == 1);
    CHECK(mid.events[0].approach_ms == 2520);
    CHECK(mid.events[0].beat_ms == 630);

    chart.control[0].speed = 23373;
    const auto slow = must_course(must_fsl(chart), 0, 0);
    REQUIRE(slow.events.size() == 1);
    CHECK(slow.events[0].approach_ms == oscilline::kMaxApproachMs);

    chart.control[0].speed = 100000;
    const auto slower = must_course(must_fsl(chart), 0, 0);
    REQUIRE(slower.events.size() == 1);
    CHECK(slower.events[0].approach_ms == oscilline::kMaxApproachMs);
    CHECK(fast.events[0].approach_ms < mid.events[0].approach_ms);
    CHECK(mid.events[0].approach_ms < slow.events[0].approach_ms);
}

TEST_CASE("negative pattern index and unknown pattern types spawn nothing") {
    Chart chart;
    chart.fixed.push_back({{4}});
    chart.control.push_back({});
    chart.patterns.push_back({0, 1, -1});
    chart.patterns.push_back({22050, 9, 0});
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 3, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 1);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[0].obstacle == 4);
}

TEST_CASE("unknown obstacle ids are skipped and a zero shadow still advances") {
    Chart chart;
    chart.fixed.push_back({{0, 99, 2}});
    Chart::Control control;
    control.shadow = 0;
    chart.control.push_back(control);
    chart.patterns.push_back({22050 * 2, 1, 0});
    chart.patterns.push_back({22050 * 2 + 3308 * 4, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() == 3);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.events[0].obstacle == 0);
    CHECK(timeline.events[1].hit_ms == 2400);
    CHECK(timeline.events[1].obstacle == 2);
    CHECK(timeline.events[2].hit_ms == 2600);
    CHECK(timeline.events[2].obstacle == 0);
}

TEST_CASE("the course ends at the later of the audio and the last obstacle") {
    Chart chart;
    chart.distributions.push_back(seed_distribution(1));
    chart.control.push_back({});
    chart.patterns.push_back({0, 2, 0});

    const auto full = must_course(must_fsl(chart), 0, 20000);
    REQUIRE(full.events.size() > 10);
    CHECK(full.events.front().hit_ms >= full.events.front().approach_ms + oscilline::kLeadInMs);
    CHECK(full.events.back().hit_ms <= 20000);
    CHECK(full.audio_end_ms == 20000);
    for (const oscilline::CourseEvent& event : full.events) {
        CHECK_FALSE(
            oscilline::obstacle_after_audio(event.hit_ms, event.approach_ms, full.audio_end_ms));
    }
    const std::int32_t full_tail = full.events.back().hit_ms + oscilline::kCourseEndTailMs;
    CHECK(full.duration_ms == std::max(20000, full_tail));
    CHECK(full.duration_ms > full.events.front().hit_ms + oscilline::kCourseEndTailMs);

    Chart early;
    early.fixed.push_back({{1}});
    early.control.push_back({});
    early.patterns.push_back({22050 * 2, 1, 0});
    early.patterns.push_back({22050 * 4, 0, 0});
    const auto stopped = must_course(must_fsl(early), 0, 20000);
    REQUIRE(stopped.events.size() == 2);
    CHECK(stopped.events[0].hit_ms == 2000);
    CHECK(stopped.events[1].hit_ms == 3000);
    CHECK(stopped.duration_ms == 20000);
    CHECK(stopped.audio_end_ms == 20000);

    const auto no_audio = must_course(must_fsl(early), 0, 0);
    CHECK(no_audio.duration_ms == 4000);
    CHECK(no_audio.audio_end_ms == 0);
}

TEST_CASE("nothing past the music is judged or missed") {
    using namespace oscilline;
    CourseTimeline late = chart_event(0, 2500, 4000);
    late.audio_end_ms = 1000;
    PlayState state;
    play_advance(state, late, 0, 3000, 0);
    CHECK(state.misses == 0);
    CHECK(state.event_index == 1);

    CourseTimeline during = chart_event(0, 1000, 4000);
    during.audio_end_ms = 5000;
    PlayState scored;
    const ObstacleWindow window = obstacle_window(during.events[0]);
    const std::int64_t perfect = (window.perfect_open + window.perfect_close) / 2;
    play_advance(scored, during, window.perfect_open, perfect, kActionBlock);
    CHECK(scored.perfects == 1);
    CHECK(scored.misses == 0);

    CourseTimeline closed = chart_event(0, 1000, 8000);
    const ObstacleWindow finished = obstacle_window(closed.events[0]);
    closed.audio_end_ms = static_cast<std::int32_t>(finished.good_close + 500);
    PlayState missed;
    play_advance(missed, closed, 0, closed.audio_end_ms + 100, 0);
    CHECK(missed.misses == 1);

    CourseTimeline straddle = chart_event(0, 1000, 4000);
    const ObstacleWindow edge = obstacle_window(straddle.events[0]);
    straddle.audio_end_ms = static_cast<std::int32_t>(edge.good_open + 1);
    REQUIRE(edge.good_close > straddle.audio_end_ms);
    PlayState dropped;
    play_advance(dropped, straddle, 0, edge.good_close + 10, 0);
    CHECK(dropped.misses == 0);
    CHECK(dropped.event_index == 1);
}

TEST_CASE("the course loader reads the script out of a language archive") {
    Chart chart;
    chart.fixed.push_back({{1}});
    chart.control.push_back({});
    chart.patterns.push_back({0, 1, 0});

    oscilline::PakArchive archive;
    oscilline::PakEntry entry;
    entry.name = "script\\system3.fsl";
    entry.data = chart_bytes(chart);
    archive.entries.push_back(std::move(entry));
    auto loaded = oscilline::load_course_script(archive);
    REQUIRE(loaded);
    const auto timeline = must_course(loaded.value(), 0, 2500);
    CHECK(timeline.cdda_track == 2);
    REQUIRE(timeline.events.size() == 1);
    CHECK(timeline.events[0].hit_ms == 2000);
    CHECK(timeline.duration_ms == 3000);

    oscilline::PakArchive empty;
    auto missing = oscilline::load_course_script(empty);
    CHECK_FALSE(missing);
    CHECK(missing.error().find("SYSTEM3.FSL") != std::string::npos);
}

TEST_CASE("a missing course index is rejected") {
    Words words;
    header(words, 1, 1, 1, 1, 0, 0);
    words.u32(0);
    words.u32(0);
    words.u32(0);
    words.u32(0);
    words.u32(0);
    words.u32(0);
    auto file = oscilline::parse_fsl(words.bytes);
    REQUIRE(file);
    CHECK_FALSE(oscilline::build_course(file.value(), 1, 0));

    oscilline::CourseMapOptions options;
    options.units_per_second = 0;
    CHECK_FALSE(oscilline::build_course(file.value(), 0, 0, options));
}

TEST_CASE("perfect ends at the front edge and good extends into the obstacle") {
    using namespace oscilline;
    const CourseTimeline block = chart_event(0, 1000, 4000);
    const CourseEvent& event = block.events[0];
    const ObstacleWindow window = obstacle_window(event);
    const float loop = static_cast<float>(kLogicalWidth) * kLoopWidthFraction;
    const float leading = obstacle_leading_x(event.obstacle, loop);
    // The arch's front sits on the origin, and perfect ends there plus the late bias.
    CHECK(leading == doctest::Approx(0.f).epsilon(0.001));
    CHECK(obstacle_perfect_anchor_x(event.obstacle, loop) == doctest::Approx(leading));
    CHECK(window.perfect_close == event.hit_ms + kJudgmentLateBiasMs);
    CHECK(window.perfect_open == window.perfect_close - kPerfectWindowMs * 2);
    const std::int64_t center = (window.perfect_open + window.perfect_close) / 2;
    CHECK(center == window.perfect_close - kPerfectWindowMs);
    CHECK(window.perfect_open < event.hit_ms);
    CHECK(window.perfect_close > event.hit_ms);
    const int early_margin = kGoodWindowMs - kPerfectWindowMs;
    CHECK(window.good_close - window.perfect_close == kGoodIntoObstacleMs);
    CHECK(window.perfect_open - window.good_open == early_margin);
    CHECK(window.good_close - window.perfect_close == window.perfect_open - window.good_open);

    PlayState state;
    const std::int64_t perfect = (window.perfect_open + window.perfect_close) / 2;
    play_advance(state, block, window.perfect_open, perfect, kActionBlock);
    CHECK(state.last == Judgment::Perfect);
    CHECK(state.score == 3);
    CHECK(state.perfects == 1);
    CHECK(state.event_index == 1);
    CHECK_FALSE(state.finished);

    state = {};
    play_advance(state, block, window.perfect_close, window.perfect_close + 1, kActionBlock);
    CHECK(state.last == Judgment::Good);
    CHECK(state.score == 2);
    CHECK(state.goods == 1);

    state = {};
    play_advance(state, block, window.good_open, perfect, kActionLoop);
    CHECK(state.last == Judgment::Miss);
    CHECK(state.score == 0);
    CHECK(state.damage == 1);
    CHECK(state.misses == 1);

    state = {};
    play_advance(state,
                 block,
                 window.good_open,
                 perfect,
                 static_cast<std::uint8_t>(kActionBlock | kActionLoop));
    CHECK(state.last == Judgment::Miss);

    state = {};
    play_advance(state, block, 0, perfect, 0);
    CHECK(state.last == Judgment::None);
    play_advance(state, block, perfect, window.good_close + 1, 0);
    CHECK(state.last == Judgment::Miss);

    state = {};
    play_advance(state, block, 0, window.good_open - 1, kActionBlock);
    CHECK(state.event_index == 0);
    play_advance(state, block, window.good_open - 1, window.good_close + 1, 0);
    CHECK(state.last == Judgment::Miss);

    state = {};
    play_advance(state, block, window.good_open, window.good_close, kActionBlock);
    CHECK(state.last == Judgment::Good);
    state = {};
    play_advance(state, block, window.good_open, window.good_close + 1, kActionBlock);
    CHECK(state.last == Judgment::Miss);

    PlayState pair;
    const CourseTimeline combo = chart_event(4, 1000, 4000);
    const ObstacleWindow pair_window = obstacle_window(combo.events[0]);
    const std::int64_t pair_perfect = (pair_window.perfect_open + pair_window.perfect_close) / 2;
    play_advance(pair, combo, pair_window.good_open, pair_perfect, kActionBlock);
    CHECK(pair.event_index == 0);
    CHECK(pair.last == Judgment::None);
    play_advance(pair, combo, pair_perfect, pair_window.perfect_close, kActionPit);
    CHECK(pair.last == Judgment::Perfect);
    CHECK(pair.score == 3);
    CHECK(pair.event_index == 1);

    // No travel time skips the leading-edge offset. Loop windows retain their small shift.
    CourseEvent still;
    still.obstacle = 0;
    still.hit_ms = 1000;
    const ObstacleWindow ended = obstacle_window(still);
    CHECK(ended.perfect_open == 1000 - kPerfectWindowMs * 2 + kJudgmentLateBiasMs);
    CHECK(ended.perfect_close == 1000 + kJudgmentLateBiasMs);
    CHECK(ended.good_open == ended.perfect_open - (kGoodWindowMs - kPerfectWindowMs));
    CHECK(ended.good_close == 1000 + kJudgmentLateBiasMs + kGoodIntoObstacleMs);
    CourseEvent still_loop;
    still_loop.obstacle = kLoopObstacleId;
    still_loop.hit_ms = 1000;
    const ObstacleWindow loop_ended = obstacle_window(still_loop);
    CHECK(loop_ended.perfect_close == ended.perfect_close + kLoopPerfectZoneShiftMs);
    CHECK(loop_ended.perfect_open == ended.perfect_open + kLoopPerfectZoneShiftMs);
    CHECK(loop_ended.good_open == ended.good_open + kLoopPerfectZoneShiftMs);
    CHECK(loop_ended.good_close == ended.good_close + kLoopPerfectZoneShiftMs);
}

TEST_CASE("one press does not clear the next obstacle") {
    using namespace oscilline;
    CourseTimeline timeline;
    timeline.duration_ms = 4000;
    for (const int hit : {1000, 1100}) {
        CourseEvent event;
        event.obstacle = 0;
        event.hit_ms = hit;
        event.approach_ms = 512;
        timeline.events.push_back(event);
    }
    PlayState state;
    const std::int64_t when = perfect_at(timeline.events[0]);
    play_advance(state, timeline, 0, when, kActionBlock);
    CHECK(state.event_index == 1);
    CHECK(state.last == Judgment::Perfect);
    const std::int64_t after = obstacle_window(timeline.events[1]).good_close + 1;
    play_advance(state, timeline, when, after, 0);
    CHECK(state.event_index == 2);
    CHECK(state.last == Judgment::Miss);
}

TEST_CASE("misses drop rabbit, frog, then worm, and out ends the course") {
    using namespace oscilline;
    const CourseTimeline misses = hits_at(kRabbitMisses + kFrogMisses + kWormMisses, 200, 20000);
    PlayState state;
    int cursor = 0;
    const auto miss_n = [&](int count) {
        for (int i = 0; i < count; ++i) {
            const ObstacleWindow window =
                obstacle_window(misses.events[static_cast<std::size_t>(cursor)]);
            play_advance(state, misses, window.good_open, window.good_close + 1, 0);
            ++cursor;
        }
    };
    miss_n(kRabbitMisses - 1);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.damage == kRabbitMisses - 1);
    miss_n(1);
    CHECK(state.form == Form::Frog);
    CHECK(state.damage == 0);
    CHECK(state.burst_until_ms ==
          obstacle_window(misses.events[static_cast<std::size_t>(kRabbitMisses - 1)]).good_close +
              1 + kFormBurstMs);
    CHECK_FALSE(state.finished);

    miss_n(kFrogMisses);
    CHECK(state.form == Form::Worm);
    CHECK(state.damage == 0);
    CHECK_FALSE(state.finished);

    miss_n(kWormMisses - 1);
    CHECK(state.form == Form::Worm);
    miss_n(1);
    CHECK(state.form == Form::Out);
    CHECK(state.finished);
    CHECK(state.misses == kRabbitMisses + kFrogMisses + kWormMisses);
}

TEST_CASE("a clear wipes damage and a clear streak raises one form") {
    using namespace oscilline;
    CourseTimeline timeline = hits_at(kClearsToRise + 2, 500, 20000);
    PlayState state;
    const ObstacleWindow first_window = obstacle_window(timeline.events[0]);
    const ObstacleWindow second_window = obstacle_window(timeline.events[1]);
    play_advance(state, timeline, 0, first_window.good_close + 1, 0);
    play_advance(state, timeline, first_window.good_close + 1, second_window.good_close + 1, 0);
    CHECK(state.damage == 2);
    play_advance(
        state, timeline, timeline.events[1].hit_ms, timeline.events[2].hit_ms, kActionBlock);
    CHECK(state.damage == 0);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.clear_run == 1);

    state = {};
    state.form = Form::Frog;
    state.event_index = 0;
    for (int i = 0; i < kClearsToRise; ++i) {
        const std::int64_t when = perfect_at(timeline.events[static_cast<std::size_t>(i)]);
        play_advance(state, timeline, when - 10, when, kActionBlock);
    }
    CHECK(state.form == Form::Rabbit);
    CHECK(state.clear_run == 0);
    CHECK(state.score == 179);

    state = {};
    state.form = Form::Worm;
    state.event_index = 0;
    const std::int64_t worm_hit = perfect_at(timeline.events[0]);
    play_advance(state, timeline, worm_hit - 10, worm_hit, kActionBlock);
    CHECK(state.score == 3);
    CHECK(state.form == Form::Worm);
}

TEST_CASE("a hit during the scribble burst is still judged") {
    using namespace oscilline;
    CourseTimeline timeline = hits_at(kRabbitMisses + 1, 200, 20000);
    PlayState state;
    for (int i = 0; i < kRabbitMisses; ++i) {
        const ObstacleWindow window = obstacle_window(timeline.events[static_cast<std::size_t>(i)]);
        play_advance(state, timeline, window.good_open, window.good_close + 1, 0);
    }
    CHECK(state.form == Form::Frog);
    CHECK(state.burst_until_ms > timeline.events[static_cast<std::size_t>(kRabbitMisses)].hit_ms);
    const std::int64_t hit = perfect_at(timeline.events[static_cast<std::size_t>(kRabbitMisses)]);
    play_advance(state, timeline, hit - 10, hit, kActionBlock);
    CHECK(state.last == Judgment::Perfect);
    CHECK(state.score == 3);
    CHECK(state.form == Form::Frog);
    CHECK_FALSE(state.finished);
}

TEST_CASE("the progress arc fill is now over duration") {
    using namespace oscilline;
    CHECK(course_progress(0, 1000) == doctest::Approx(0.f));
    CHECK(course_progress(250, 1000) == doctest::Approx(0.25f));
    CHECK(course_progress(1000, 1000) == doctest::Approx(1.f));
    CHECK(course_progress(1500, 1000) == doctest::Approx(1.f));
    CHECK(course_progress(-5, 1000) == doctest::Approx(0.f));
    CHECK(course_progress(10, 0) == doctest::Approx(1.f));
}

TEST_CASE("pause ignores the clock and the course ends after the last event") {
    using namespace oscilline;
    auto block = chart_event(0, 1000, 2000);
    PlayState state;
    state.paused = true;
    play_advance(state, block, 0, 1000, kActionBlock);
    CHECK(state.score == 0);
    CHECK(state.event_index == 0);
    CHECK(state.last == Judgment::None);

    state.paused = false;
    play_advance(state, block, 0, 1000, kActionBlock);
    CHECK_FALSE(state.finished);
    play_advance(state, block, 1000, 2000, 0);
    CHECK(state.finished);

    PlayState backward;
    play_advance(backward, block, 1500, 1000, kActionBlock);
    CHECK(backward.event_index == 0);
}

TEST_CASE("a course ends at its duration without missing a later obstacle") {
    using namespace oscilline;
    CourseTimeline timeline;
    timeline.duration_ms = 2000;
    CourseEvent later;
    later.obstacle = 0;
    later.hit_ms = 5000;
    later.approach_ms = 1000;
    timeline.events.push_back(later);

    PlayState state;
    play_advance(state, timeline, 0, 2000, 0);
    CHECK(state.finished);
    CHECK(state.event_index == 0);
    CHECK(state.last == Judgment::None);
    CHECK(state.score == 0);

    PlayState open;
    auto block = chart_event(0, 1000, 1000);
    play_advance(open, block, 0, 1000, 0);
    CHECK_FALSE(open.finished);
    CHECK(open.last == Judgment::None);
    const ObstacleWindow open_window = obstacle_window(block.events[0]);
    play_advance(open, block, 1000, open_window.good_close + 1, 0);
    CHECK(open.last == Judgment::Miss);
    CHECK(open.finished);
}

TEST_CASE("per-segment scroll speed keeps screen x in hit order") {
    using namespace oscilline;
    Chart chart;
    chart.fixed.push_back({{0, 1, 2, 3}});
    Chart::Control slow;
    slow.speed = 22050 * 7 / 10;
    slow.shadow = 1;
    chart.control.push_back(slow);
    Chart::Control fast;
    fast.start = 22050 * 8;
    fast.speed = 22050 / 5;
    fast.shadow = 1;
    chart.control.push_back(fast);
    chart.patterns.push_back({22050 * 3, 1, 0});
    chart.patterns.push_back({22050 * 14, 0, 0});

    const auto timeline = must_course(must_fsl(chart), 0, 0);
    REQUIRE(timeline.events.size() > 20);
    int slow_speed = -1;
    int fast_speed = -1;
    for (const CourseEvent& event : timeline.events) {
        CHECK(event.scroll_approach_ms > 0);
        if (event.hit_ms < 8000) {
            if (slow_speed < 0) {
                slow_speed = event.scroll_approach_ms;
            }
            CHECK(event.scroll_approach_ms == slow_speed);
        } else {
            if (fast_speed < 0) {
                fast_speed = event.scroll_approach_ms;
            }
            CHECK(event.scroll_approach_ms == fast_speed);
        }
    }
    REQUIRE(slow_speed > 2500);
    REQUIRE(fast_speed > 0);
    CHECK(fast_speed < 1100);
    CHECK(slow_speed != fast_speed);

    constexpr float kWidth = 640.f;
    const float hit_x = kWidth * kHitXFraction;
    const float spawn_x = kWidth * kSpawnXFraction;
    for (std::int64_t t = 0; t <= timeline.duration_ms; t += 50) {
        float previous = -1.0e9f;
        for (const CourseEvent& event : timeline.events) {
            if (t > event.hit_ms) {
                continue;
            }
            const float x =
                obstacle_screen_x(hit_x, spawn_x, event.scroll_approach_ms, event.hit_ms, t);
            CHECK(x + 0.05f >= previous);
            previous = x;
        }
    }
}

TEST_CASE("the camera intro, play, and outro follow their constants") {
    using namespace oscilline;
    constexpr float kWidth = 640.f;
    constexpr std::int32_t kEnd = 20000;
    const CameraPose start = camera_at(0, kEnd, kWidth);
    CHECK(start.phase == CameraPhase::Intro);
    CHECK(start.tilt_deg == doctest::Approx(kCameraIntroTiltDeg));
    CHECK(start.figure_x == doctest::Approx(kWidth * kCameraIntroCenterFraction));

    const CameraPose late_intro = camera_at(kCameraIntroMs - 1, kEnd, kWidth);
    CHECK(late_intro.phase == CameraPhase::Intro);
    CHECK(late_intro.tilt_deg > 0.f);
    CHECK(late_intro.tilt_deg < kCameraIntroTiltDeg);
    CHECK(late_intro.figure_x > kWidth * kHitXFraction);
    CHECK(late_intro.figure_x < kWidth * kCameraIntroCenterFraction);

    const CameraPose play = camera_at(kCameraIntroMs, kEnd, kWidth);
    CHECK(play.phase == CameraPhase::Play);
    CHECK(play.tilt_deg == doctest::Approx(0.f));
    CHECK(play.figure_x == doctest::Approx(kWidth * kHitXFraction));

    const CameraPose later = camera_at((kCameraIntroMs + kEnd) / 2, kEnd, kWidth);
    CHECK(later.phase == CameraPhase::Play);
    CHECK(later.tilt_deg == doctest::Approx(play.tilt_deg));
    CHECK(later.figure_x == doctest::Approx(play.figure_x));

    const CameraPose outro = camera_at(kEnd, kEnd, kWidth);
    CHECK(outro.phase == CameraPhase::Outro);
    CHECK(outro.tilt_deg == doctest::Approx(0.f));
    CHECK(outro.figure_x == doctest::Approx(kWidth * kHitXFraction));

    const CameraPose done =
        camera_at(static_cast<std::int64_t>(kEnd) + kCameraOutroMs, kEnd, kWidth);
    CHECK(done.phase == CameraPhase::Outro);
    CHECK(done.tilt_deg == doctest::Approx(kCameraIntroTiltDeg));
    CHECK(done.figure_x == doctest::Approx(kWidth * kCameraIntroCenterFraction));
}

TEST_CASE("ribbon vibration follows the onset envelope and then decays") {
    using namespace oscilline;
    CHECK(ribbon_vibrate_px({}, 20, 480.f) == doctest::Approx(0.f));
    std::vector<float> impulse(80, 0.f);
    impulse[0] = 40.f;
    const float early = ribbon_vibrate_px(impulse, 20, 480.f);
    const float later = ribbon_vibrate_px(impulse, 20 + 417, 480.f);
    CHECK(std::fabs(early) > 1.f);
    CHECK(std::fabs(later) < std::fabs(early) * 0.2f);

    std::vector<float> quiet(80, 0.f);
    quiet[2] = 0.5f;
    CHECK(std::fabs(ribbon_vibrate_px(quiet, 20, 480.f)) < std::fabs(early));
    CHECK(ribbon_vibrate_px(std::vector<float>(80, 0.f), 20, 480.f) == doctest::Approx(0.f));
}

TEST_CASE("ribbon jitter amplitude follows damage and resets on a form change") {
    using namespace oscilline;
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 0, -1, 0) ==
          doctest::Approx(kRibbonJitterRestPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 1, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakLowPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Frog, 3, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakLowPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 4, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakHighPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Frog, 9, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakHighPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Worm, 1, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakWormPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Worm, 4, 0, 0) ==
          doctest::Approx(kRibbonJitterPeakWormPs));

    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 2, 0, 1600) ==
          doctest::Approx(kRibbonJitterPeakLowPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 2, 0, 2000) == doctest::Approx(3.5f));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 2, 0, 2400) ==
          doctest::Approx(kRibbonJitterRestPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 2, 0, 4000) ==
          doctest::Approx(kRibbonJitterRestPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 4, 0, 2000) == doctest::Approx(6.f));
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 4, 0, 2400) ==
          doctest::Approx(kRibbonJitterFloorHighPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Worm, 2, 0, 2400) ==
          doctest::Approx(kRibbonJitterRestPs));
    CHECK(ribbon_jitter_amplitude_ps(Form::Worm, 5, 0, 2400) ==
          doctest::Approx(kRibbonJitterFloorHighPs));
    // One 50 Hz sample: 1599 and 1580 share the hold, which is still the peak.
    CHECK(ribbon_jitter_amplitude_ps(Form::Rabbit, 1, 0, 1599) ==
          doctest::Approx(kRibbonJitterPeakLowPs));
    CHECK(ribbon_jitter_time(1000) == 1000);
    CHECK(ribbon_jitter_time(1019) == 1000);
    CHECK(ribbon_jitter_time(1020) == 1020);

    const CourseTimeline misses = hits_at(kRabbitMisses + 6, 200, 40000);
    PlayState state;
    const auto miss_at = [&](int index) {
        const ObstacleWindow window =
            obstacle_window(misses.events[static_cast<std::size_t>(index)]);
        play_advance(state, misses, window.good_open, window.good_close + 1, 0);
    };
    miss_at(0);
    CHECK(state.hits_since_form == 1);
    CHECK(state.last_hit_ms == obstacle_window(misses.events[0]).good_close + 1);
    CHECK(state.form == Form::Rabbit);
    for (int i = 1; i < 4; ++i) {
        miss_at(i);
    }
    CHECK(state.hits_since_form == 4);
    const int saved_hits = state.hits_since_form;
    const std::int64_t saved_hit = state.last_hit_ms;
    const std::int64_t clear_hit = perfect_at(misses.events[4]);
    play_advance(state, misses, clear_hit - 10, clear_hit, kActionBlock);
    CHECK(state.last == Judgment::Perfect);
    CHECK(state.damage == 0);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.hits_since_form == saved_hits);
    CHECK(state.last_hit_ms == saved_hit);
    const float held = ribbon_jitter_amplitude_ps(
        state.form, state.hits_since_form, state.last_hit_ms, state.last_hit_ms);
    CHECK(held == doctest::Approx(kRibbonJitterPeakHighPs));

    int drops = 0;
    for (int i = 5; i < static_cast<int>(misses.events.size()) && state.form == Form::Rabbit; ++i) {
        miss_at(i);
        ++drops;
    }
    CHECK(drops == kRabbitMisses);
    CHECK(state.form == Form::Frog);
    CHECK(state.hits_since_form == 0);
    CHECK(state.last_hit_ms < 0);
    CHECK(ribbon_jitter_amplitude_ps(state.form, state.hits_since_form, state.last_hit_ms, 0) ==
          doctest::Approx(kRibbonJitterRestPs));

    CourseTimeline rises = hits_at(kClearsToRise + 1, 500, 20000);
    PlayState worm;
    worm.form = Form::Worm;
    worm.hits_since_form = 2;
    worm.last_hit_ms = 100;
    const std::int32_t first = rises.events[0].hit_ms;
    play_advance(worm, rises, first - 10, first, kActionBlock);
    CHECK(worm.form == Form::Worm);
    CHECK(worm.hits_since_form == 2);
    CHECK(worm.last_hit_ms == 100);
    for (int i = 1; i < kClearsToRise; ++i) {
        const std::int32_t hit = rises.events[static_cast<std::size_t>(i)].hit_ms;
        play_advance(worm, rises, hit - 10, hit, kActionBlock);
    }
    CHECK(worm.form == Form::Frog);
    CHECK(worm.hits_since_form == 0);
    CHECK(worm.last_hit_ms < 0);
    CHECK(ribbon_jitter_amplitude_ps(worm.form, worm.hits_since_form, worm.last_hit_ms, 100) ==
          doctest::Approx(kRibbonJitterRestPs));
}

TEST_CASE("segmented ribbon jitter is bounded, straight, and not a stage offset") {
    using namespace oscilline;
    constexpr float kLeft = 0.f;
    constexpr float kRight = 640.f;
    constexpr float kBaseline = 240.f;
    constexpr std::int64_t kTime = 1000;
    const float rest_y = kRibbonJitterRestPs * kPlayStationPixelToLogicalY;
    const float attachments[] = {400.f, 100.f, 250.f, -50.f, 700.f};
    const std::vector<Segment> spine =
        ribbon_jitter_spine(kLeft, kRight, kBaseline, attachments, kTime, kRibbonJitterRestPs, 2.f);
    // Edges plus the three on-screen attachments. Off-screen x is not a break.
    const float breaks[] = {0.f, 100.f, 250.f, 400.f, 640.f};
    REQUIRE(spine.size() == 4);
    for (std::size_t i = 0; i < spine.size(); ++i) {
        CHECK(spine[i].x0 == doctest::Approx(breaks[i]));
        CHECK(spine[i].x1 == doctest::Approx(breaks[i + 1]));
        CHECK(std::fabs(spine[i].y0 - kBaseline) <= rest_y + 1.e-3f);
        CHECK(std::fabs(spine[i].y1 - kBaseline) <= rest_y + 1.e-3f);
        const float mid_x = (spine[i].x0 + spine[i].x1) * 0.5f;
        const float mid_y = (spine[i].y0 + spine[i].y1) * 0.5f;
        const float dx = spine[i].x1 - spine[i].x0;
        const float dy = spine[i].y1 - spine[i].y0;
        const float cross = dx * (mid_y - spine[i].y0) - dy * (mid_x - spine[i].x0);
        CHECK(std::fabs(cross) < 1.e-3f);
        if (i + 1 < spine.size()) {
            CHECK(spine[i].x1 == doctest::Approx(spine[i + 1].x0));
            CHECK(spine[i].y1 == doctest::Approx(spine[i + 1].y0));
        }
    }
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (const Segment& segment : spine) {
        lo = std::min(lo, std::min(segment.y0, segment.y1));
        hi = std::max(hi, std::max(segment.y0, segment.y1));
    }
    CHECK(hi - lo > 0.05f);

    const std::vector<Segment> again = ribbon_jitter_spine(
        kLeft, kRight, kBaseline, attachments, kTime + 19, kRibbonJitterRestPs, 2.f);
    REQUIRE(again.size() == spine.size());
    for (std::size_t i = 0; i < spine.size(); ++i) {
        CHECK(again[i].y0 == doctest::Approx(spine[i].y0));
        CHECK(again[i].y1 == doctest::Approx(spine[i].y1));
    }
    const std::vector<Segment> next = ribbon_jitter_spine(
        kLeft, kRight, kBaseline, attachments, kTime + 20, kRibbonJitterRestPs, 2.f);
    bool moved = false;
    for (std::size_t i = 0; i < spine.size(); ++i) {
        moved = moved || std::fabs(next[i].y0 - spine[i].y0) > 1.e-3f;
    }
    CHECK(moved);

    bool signed_spread = false;
    float previous = ribbon_jitter_offset(kTime, 1u, rest_y);
    for (std::uint32_t channel = 0; channel < 24; ++channel) {
        const float offset = ribbon_jitter_offset(kTime, channel, rest_y);
        CHECK(std::fabs(offset) <= rest_y + 1.e-4f);
        CHECK(ribbon_jitter_offset(kTime + 19, channel, rest_y) == doctest::Approx(offset));
        if (offset * previous < 0.f) {
            signed_spread = true;
        }
        previous = offset;
    }
    CHECK(signed_spread);
    CHECK(ribbon_jitter_offset(kTime, 3u, 0.f) == doctest::Approx(0.f));

    const float peak_y = kRibbonJitterPeakLowPs * kPlayStationPixelToLogicalY;
    const float peak_x = kRibbonJitterPeakLowPs * kPlayStationPixelToLogicalX;
    for (std::uint8_t id = 0; id < 10; ++id) {
        const RibbonAttachment attachment = obstacle_ribbon_attachment(id, 96.f);
        REQUIRE(attachment.valid);
        CHECK(attachment.left < attachment.right);
        std::vector<Segment> obstacle;
        constexpr float kOrigin = 320.f;
        append_jittered_obstacle(
            obstacle, id, kOrigin, kBaseline, 96.f, 1.f, kTime, kRibbonJitterPeakLowPs, id + 1u);
        REQUIRE(!obstacle.empty());
        const float left_x = kOrigin + attachment.left;
        const float right_x = kOrigin + attachment.right;
        const float left_y = ribbon_break_y(kBaseline, left_x, kTime, peak_y);
        const float right_y = ribbon_break_y(kBaseline, right_x, kTime, peak_y);
        const float xs[] = {left_x, right_x};
        const std::vector<Segment> joined =
            ribbon_jitter_spine(kLeft, kRight, kBaseline, xs, kTime, kRibbonJitterPeakLowPs, 2.f);
        bool spine_left = false;
        bool spine_right = false;
        for (const Segment& segment : joined) {
            if (std::fabs(segment.x0 - left_x) < 1.e-3f &&
                std::fabs(segment.y0 - left_y) < 1.e-3f) {
                spine_left = true;
            }
            if (std::fabs(segment.x1 - left_x) < 1.e-3f &&
                std::fabs(segment.y1 - left_y) < 1.e-3f) {
                spine_left = true;
            }
            if (std::fabs(segment.x0 - right_x) < 1.e-3f &&
                std::fabs(segment.y0 - right_y) < 1.e-3f) {
                spine_right = true;
            }
            if (std::fabs(segment.x1 - right_x) < 1.e-3f &&
                std::fabs(segment.y1 - right_y) < 1.e-3f) {
                spine_right = true;
            }
        }
        CHECK(spine_left);
        CHECK(spine_right);
        bool end_left = false;
        bool end_right = false;
        for (const Segment& segment : obstacle) {
            const auto pin = [&](float x, float y, float px, float py) {
                return std::fabs(x - px) < 1.e-3f && std::fabs(y - py) < 1.e-3f;
            };
            end_left = end_left || pin(segment.x0, segment.y0, left_x, left_y) ||
                       pin(segment.x1, segment.y1, left_x, left_y);
            end_right = end_right || pin(segment.x0, segment.y0, right_x, right_y) ||
                        pin(segment.x1, segment.y1, right_x, right_y);
        }
        CHECK(end_left);
        CHECK(end_right);
    }

    const std::vector<Segment> plain = obstacle_shape(0, 96.f);
    std::vector<Segment> block;
    append_jittered_obstacle(
        block, 0, 320.f, kBaseline, 96.f, 1.f, kTime, kRibbonJitterPeakLowPs, 9u);
    REQUIRE(block.size() == plain.size());
    bool interior_x = false;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        const auto check_end = [&](float lx, float ly, float sx, float sy) {
            const float dx = sx - (320.f + lx);
            const float dy = sy - (kBaseline + ly);
            CHECK(std::fabs(dx) <= peak_x + 1.e-3f);
            CHECK(std::fabs(dy) <= peak_y + 1.e-3f);
            if (std::fabs(ly) > 0.05f && std::fabs(dx) > 0.05f) {
                interior_x = true;
            }
        };
        check_end(plain[i].x0, plain[i].y0, block[i].x0, block[i].y0);
        check_end(plain[i].x1, plain[i].y1, block[i].x1, block[i].y1);
    }
    CHECK(interior_x);

    std::vector<Segment> figure;
    placeholder_figure(figure, Form::Rabbit, 80.f, kBaseline);
    std::vector<Segment> clean = figure;
    jitter_figure_vertices(clean, kTime, kRibbonJitterRestPs, 1u);
    REQUIRE(clean.size() == figure.size());
    for (std::size_t i = 0; i < figure.size(); ++i) {
        CHECK(clean[i].x0 == doctest::Approx(figure[i].x0));
        CHECK(clean[i].y0 == doctest::Approx(figure[i].y0));
        CHECK(clean[i].x1 == doctest::Approx(figure[i].x1));
        CHECK(clean[i].y1 == doctest::Approx(figure[i].y1));
    }
    const float extra = (kRibbonJitterPeakHighPs - kRibbonJitterRestPs) * kFigureJitterScale;
    const float fig_x = extra * kPlayStationPixelToLogicalX;
    const float fig_y = extra * kPlayStationPixelToLogicalY;
    std::vector<Segment> damaged = figure;
    jitter_figure_vertices(damaged, kTime, kRibbonJitterPeakHighPs, 1u);
    float first_dy = damaged[0].y0 - figure[0].y0;
    bool varied = false;
    bool any = false;
    for (std::size_t i = 0; i < figure.size(); ++i) {
        const auto check_end = [&](float ox, float oy, float sx, float sy) {
            const float dx = sx - ox;
            const float dy = sy - oy;
            CHECK(std::fabs(dx) <= fig_x + 1.e-3f);
            CHECK(std::fabs(dy) <= fig_y + 1.e-3f);
            any = any || std::fabs(dx) > 0.05f || std::fabs(dy) > 0.05f;
            if (std::fabs(dy - first_dy) > 0.05f) {
                varied = true;
            }
        };
        check_end(figure[i].x0, figure[i].y0, damaged[i].x0, damaged[i].y0);
        check_end(figure[i].x1, figure[i].y1, damaged[i].x1, damaged[i].y1);
    }
    CHECK(any);
    CHECK(varied);
}

TEST_CASE("figure jitter is capped below the ribbon's while the ribbon keeps its peak") {
    using namespace oscilline;
    constexpr float kBaseline = 240.f;
    const float cap_ps = (kRibbonJitterPeakHighPs - kRibbonJitterRestPs) * kFigureJitterScale;
    CHECK(cap_ps == doctest::Approx(3.f).epsilon(0.05));
    std::vector<Segment> figure;
    placeholder_figure(figure, Form::Rabbit, 80.f, kBaseline);
    float most_x = 0.f;
    float most_y = 0.f;
    float ribbon_most = 0.f;
    const float peak_y = kRibbonJitterPeakHighPs * kPlayStationPixelToLogicalY;
    for (std::int64_t time = 0; time < 4000; time += kRibbonJitterFrameMs) {
        std::vector<Segment> damaged = figure;
        jitter_figure_vertices(damaged, time, kRibbonJitterPeakHighPs, 1u);
        for (std::size_t i = 0; i < figure.size(); ++i) {
            most_x = std::max(most_x, std::fabs(damaged[i].x0 - figure[i].x0));
            most_y = std::max(most_y, std::fabs(damaged[i].y0 - figure[i].y0));
        }
        ribbon_most = std::max(
            ribbon_most, std::fabs(ribbon_break_y(kBaseline, 100.f, time, peak_y) - kBaseline));
    }
    CHECK(most_x <= cap_ps * kPlayStationPixelToLogicalX + 1.e-3f);
    CHECK(most_y <= cap_ps * kPlayStationPixelToLogicalY + 1.e-3f);
    CHECK(most_x > 0.8f * cap_ps * kPlayStationPixelToLogicalX);
    CHECK(most_y > 0.8f * cap_ps * kPlayStationPixelToLogicalY);
    CHECK(ribbon_most > 0.8f * peak_y);
}

float shape_width(const std::vector<oscilline::Segment>& segments) {
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (const oscilline::Segment& segment : segments) {
        lo = std::min(lo, std::min(segment.x0, segment.x1));
        hi = std::max(hi, std::max(segment.x0, segment.x1));
    }
    return hi - lo;
}

float shape_min_y(const std::vector<oscilline::Segment>& segments) {
    float y = 1.0e9f;
    for (const oscilline::Segment& segment : segments) {
        y = std::min(y, std::min(segment.y0, segment.y1));
    }
    return y;
}

float shape_max_y(const std::vector<oscilline::Segment>& segments) {
    float y = -1.0e9f;
    for (const oscilline::Segment& segment : segments) {
        y = std::max(y, std::max(segment.y0, segment.y1));
    }
    return y;
}

bool near_point(float x, float y, float px, float py) {
    return std::hypot(x - px, y - py) < 1.e-3f;
}

// Walk a single chain and count interior peaks above and below `mid`.
void count_chain_peaks(const std::vector<oscilline::Segment>& shape,
                       float mid,
                       int& above,
                       int& below) {
    above = 0;
    below = 0;
    REQUIRE(shape.size() >= 2);
    std::vector<float> xs;
    std::vector<float> ys;
    xs.push_back(shape.front().x0);
    ys.push_back(shape.front().y0);
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i > 0) {
            CHECK(shape[i].x0 == doctest::Approx(shape[i - 1].x1));
            CHECK(shape[i].y0 == doctest::Approx(shape[i - 1].y1));
        }
        xs.push_back(shape[i].x1);
        ys.push_back(shape[i].y1);
    }
    for (std::size_t i = 1; i + 1 < ys.size(); ++i) {
        if (ys[i] < ys[i - 1] && ys[i] < ys[i + 1] && ys[i] < mid) {
            ++above;
        }
        if (ys[i] > ys[i - 1] && ys[i] > ys[i + 1] && ys[i] > mid) {
            ++below;
        }
    }
}

// The two arms of the bottom X. Each upper end lies on the circle of `radius`
// about (`cx`, `cy`), and the arms cross under that circle on its center line.
void check_bottom_cross(const std::vector<oscilline::Segment>& shape,
                        float cx,
                        float cy,
                        float radius) {
    using oscilline::Segment;
    std::vector<Segment> arms;
    const float floor_y = cy + radius;
    for (std::size_t index = 0; index < shape.size(); ++index) {
        const Segment& segment = shape[index];
        const bool below0 = segment.y0 > floor_y + 0.05f;
        const bool below1 = segment.y1 > floor_y + 0.05f;
        if (below0 == below1) {
            continue;
        }
        arms.push_back(segment);
        const float upper_x = below0 ? segment.x1 : segment.x0;
        const float upper_y = below0 ? segment.y1 : segment.y0;
        CHECK(std::hypot(upper_x - cx, upper_y - cy) == doctest::Approx(radius).epsilon(0.001));
        bool shared = false;
        for (std::size_t other = 0; other < shape.size(); ++other) {
            if (other == index) {
                continue;
            }
            shared = shared || near_point(upper_x, upper_y, shape[other].x0, shape[other].y0) ||
                     near_point(upper_x, upper_y, shape[other].x1, shape[other].y1);
        }
        CHECK(shared);
    }
    REQUIRE(arms.size() == 2);
    const float rx = arms[0].x1 - arms[0].x0;
    const float ry = arms[0].y1 - arms[0].y0;
    const float sx = arms[1].x1 - arms[1].x0;
    const float sy = arms[1].y1 - arms[1].y0;
    const float den = rx * sy - ry * sx;
    REQUIRE(std::fabs(den) > 1.e-4f);
    const float t = ((arms[1].x0 - arms[0].x0) * sy - (arms[1].y0 - arms[0].y0) * sx) / den;
    const float u = ((arms[1].x0 - arms[0].x0) * ry - (arms[1].y0 - arms[0].y0) * rx) / den;
    CHECK(t > 0.05f);
    CHECK(t < 0.95f);
    CHECK(u > 0.05f);
    CHECK(u < 0.95f);
    const float ix = arms[0].x0 + t * rx;
    const float iy = arms[0].y0 + t * ry;
    CHECK(ix == doctest::Approx(cx).epsilon(0.001));
    CHECK(iy > cy + radius);
    // The crossing is above the ribbon. The feet, not the circle, meet y = 0.
    CHECK(iy < 0.f);
}

float point_segment_distance(float px, float py, const oscilline::Segment& segment) {
    const float dx = segment.x1 - segment.x0;
    const float dy = segment.y1 - segment.y0;
    const float len2 = dx * dx + dy * dy;
    float t = 0.f;
    if (len2 > 1.e-12f) {
        t = ((px - segment.x0) * dx + (py - segment.y0) * dy) / len2;
        if (t < 0.f) {
            t = 0.f;
        } else if (t > 1.f) {
            t = 1.f;
        }
    }
    const float qx = segment.x0 + t * dx;
    const float qy = segment.y0 + t * dy;
    return std::hypot(px - qx, py - qy);
}

float nearest_segment(const std::vector<oscilline::Segment>& shape, float x, float y) {
    float best = 1.0e9f;
    for (const oscilline::Segment& segment : shape) {
        best = std::min(best, point_segment_distance(x, y, segment));
    }
    return best;
}

// The bottom of the circle is missing, and every circle sample stays above the ribbon.
void check_circle_open_above_ribbon(const std::vector<oscilline::Segment>& shape,
                                    float cx,
                                    float cy,
                                    float radius) {
    CHECK(nearest_segment(shape, cx, cy + radius) > radius * 0.15f);
    for (const oscilline::Segment& segment : shape) {
        const float ends[2][2] = {{segment.x0, segment.y0}, {segment.x1, segment.y1}};
        for (const auto& end : ends) {
            if (std::fabs(std::hypot(end[0] - cx, end[1] - cy) - radius) > 0.05f) {
                continue;
            }
            CHECK(end[1] < -radius * 0.5f);
        }
    }
}

TEST_CASE("obstacle ids 0 through 9 are merged outlines scaled from the loop") {
    using namespace oscilline;
    constexpr float kLoop = 96.f;
    const auto loop = obstacle_shape(2, kLoop);
    REQUIRE(loop.size() >= 8);
    CHECK(shape_width(loop) == doctest::Approx(kLoop * kLoopVisualScale));
    const auto loop_wave = obstacle_shape(9, kLoop);
    CHECK(shape_width(loop_wave) > shape_width(loop));
    for (std::uint8_t id = 0; id < 10; ++id) {
        CHECK(obstacle_shape(id, kLoop).size() >= 2);
    }
    CHECK(obstacle_shape(10, kLoop).empty());
    CHECK(obstacle_shape(2, 0.f).empty());
}

TEST_CASE("level complete waits for the final results screen") {
    using namespace oscilline;
    CHECK_FALSE(course_failure_cue(Form::Rabbit).has_value());
    CHECK_FALSE(course_results_cue(Form::Rabbit, false).has_value());
    CHECK_FALSE(course_results_cue(Form::Super, false).has_value());

    const auto cleared = course_results_cue(Form::Rabbit, true);
    REQUIRE(cleared.has_value());
    CHECK(*cleared == SfxId::LevelComplete);
    const auto single = course_results_cue(Form::Frog, true);
    REQUIRE(single.has_value());
    CHECK(*single == SfxId::LevelComplete);

    const auto failed = course_failure_cue(Form::Out);
    REQUIRE(failed.has_value());
    CHECK(*failed == SfxId::GameOver);
    CHECK_FALSE(course_results_cue(Form::Out, true).has_value());
}

TEST_CASE("loop-based pair success zones sit on their cross like the standalone loop") {
    using namespace oscilline;
    CourseEvent base_loop;
    base_loop.obstacle = kLoopObstacleId;
    base_loop.hit_ms = 2500;
    base_loop.approach_ms = 2000;
    base_loop.scroll_approach_ms = 2000;
    const ObstacleWindow base_window = obstacle_window(base_loop);
    const float loop_px = static_cast<float>(kLogicalWidth) * kLoopWidthFraction;
    const double px_per_ms = static_cast<double>(kLogicalWidth) *
                             (kSpawnXFraction - kHitXFraction) /
                             stage_scroll_approach_ms(base_loop);

    // The plain loop's foot is its origin. Loop+pit's leftmost point is the same
    // circle edge, so its foot sits that far right. Loop+wave's side spikes
    // reach further left, so its cross sits further right still.
    CHECK(obstacle_loop_foot_x(kLoopObstacleId, loop_px) == doctest::Approx(0.f));
    const float pit_foot = obstacle_loop_foot_x(kLoopPitObstacleId, loop_px);
    const float wave_foot = obstacle_loop_foot_x(kLoopWaveObstacleId, loop_px);
    CHECK(pit_foot == doctest::Approx(-obstacle_leading_x(kLoopObstacleId, loop_px)));
    CHECK(wave_foot > pit_foot);
    CHECK(obstacle_loop_foot_x(5, loop_px) == 0.f);

    for (const std::uint8_t id : {kLoopPitObstacleId, kLoopWaveObstacleId}) {
        CourseEvent combo = base_loop;
        combo.obstacle = id;
        const ObstacleWindow combo_window = obstacle_window(combo);
        // Same zone, moved by the time the cross takes to cover its offset.
        const double foot_ms = obstacle_loop_foot_x(id, loop_px) / px_per_ms;
        CHECK(std::llabs(combo_window.perfect_close - base_window.perfect_close -
                         std::llround(foot_ms)) <= 1);
        CHECK(combo_window.perfect_close - combo_window.perfect_open ==
              base_window.perfect_close - base_window.perfect_open);
        CHECK(combo_window.good_close - combo_window.good_open ==
              base_window.good_close - base_window.good_open);
        CHECK(combo_window.perfect_close > base_window.perfect_close);
    }
}

TEST_CASE("the loop cross is shorter and wider and perfect is the left foot") {
    using namespace oscilline;
    constexpr float kLoop = 96.f;
    constexpr float kPi = std::numbers::pi_v<float>;
    CHECK(kLoopCrossSinPi8 == doctest::Approx(std::sin(kPi / 8.f)));
    CHECK(kLoopCrossCosPi8 == doctest::Approx(std::cos(kPi / 8.f)));
    const float old_span = 2.f * (1.f + kLoopCrossDropFraction - kLoopCrossCosPi8);
    CHECK(kLoopCrossSpanScale == doctest::Approx(0.75f));
    CHECK(kLoopCrossFootScale == doctest::Approx(1.5f));
    CHECK(kLoopCrossSpanFraction == doctest::Approx(old_span * kLoopCrossSpanScale));
    CHECK(kLoopCrossFootHalfFraction == doctest::Approx(kLoopCrossSinPi8 * kLoopCrossFootScale));
    CHECK(kLoopRibbonLift == doctest::Approx(kLoopCrossSpanFraction + kLoopCrossCosPi8));
    CHECK(kLoopPerfectCenterFraction == doctest::Approx((1.f - kLoopCrossFootHalfFraction) * 0.5f));
    CHECK(kLoopCrossSpanFraction < old_span);
    CHECK(kLoopCrossFootHalfFraction > kLoopCrossSinPi8);

    const float radius = kLoop * kLoopVisualScale * kLoopRadiusFraction;
    const std::vector<Segment> loop = obstacle_shape(kLoopObstacleId, kLoop);
    const float width = obstacle_width(kLoopObstacleId, kLoop);
    const float leading = obstacle_leading_x(kLoopObstacleId, kLoop);
    CHECK(width == doctest::Approx(radius * 2.f));
    const float anchor = obstacle_perfect_anchor_x(kLoopObstacleId, kLoop);
    CHECK(anchor == doctest::Approx(leading + kLoopPerfectCenterFraction * width));
    CHECK(std::fabs(anchor) < 1.e-3f);
    const float cx = leading + radius;
    const float cy = -kLoopRibbonLift * radius;
    const float foot = radius * kLoopCrossFootHalfFraction;
    // The outline is seated on the left foot. The circle's center is not.
    CHECK(cx == doctest::Approx(foot));
    CHECK(std::fabs(cx - anchor) > 0.2f * radius);
    float left_foot = 1.0e9f;
    float right_foot = -1.0e9f;
    int ribbon_ends = 0;
    float join_y = -1.0e9f;
    for (const Segment& segment : loop) {
        const float ends[2][2] = {{segment.x0, segment.y0}, {segment.x1, segment.y1}};
        for (const auto& end : ends) {
            if (std::fabs(end[1]) <= 1.e-3f) {
                left_foot = std::min(left_foot, end[0]);
                right_foot = std::max(right_foot, end[0]);
                ++ribbon_ends;
            }
            if (std::fabs(std::hypot(end[0] - cx, end[1] - cy) - radius) < 0.05f) {
                join_y = std::max(join_y, end[1]);
            }
        }
    }
    CHECK(ribbon_ends == 2);
    CHECK(left_foot == doctest::Approx(anchor).epsilon(0.001));
    CHECK(right_foot - left_foot == doctest::Approx(2.f * foot));
    CHECK(join_y == doctest::Approx(-radius * kLoopCrossSpanFraction));
    CHECK(shape_min_y(loop) < cy - radius + 0.05f);
    CHECK(shape_max_y(loop) == doctest::Approx(0.f).epsilon(0.001));
    check_bottom_cross(loop, cx, cy, radius);
    check_circle_open_above_ribbon(loop, cx, cy, radius);

    for (std::uint8_t id = 0; id < 10; ++id) {
        const auto shape = obstacle_shape(id, kLoop);
        REQUIRE(!shape.empty());
        for (const Segment& segment : shape) {
            CHECK(segment.color.r == doctest::Approx(1.f));
            CHECK(segment.color.g == doctest::Approx(1.f));
            CHECK(segment.color.b == doctest::Approx(1.f));
        }
        CHECK(std::fabs(obstacle_perfect_anchor_x(id, kLoop)) < 1.e-3f);
        CourseEvent event;
        event.obstacle = id;
        event.hit_ms = 2500;
        event.approach_ms = 2000;
        event.scroll_approach_ms = 2000;
        const ObstacleWindow window = obstacle_window(event);
        const float screen_w = static_cast<float>(kLogicalWidth);
        const float span = screen_w * (kSpawnXFraction - kHitXFraction);
        const bool loop_pair = obstacle_is_pair(id) && (obstacle_actions(id) & kActionLoop) != 0 &&
                               (obstacle_actions(id) & kActionBlock) == 0;
        const std::uint8_t zone_obstacle = loop_pair ? kLoopObstacleId : id;
        const float lead_px =
            obstacle_leading_x(zone_obstacle, screen_w * kLoopWidthFraction) +
            obstacle_loop_foot_x(loop_pair ? id : zone_obstacle, screen_w * kLoopWidthFraction);
        const double px_per_ms = static_cast<double>(span) / stage_scroll_approach_ms(event);
        const std::int64_t lead_ms = std::llround(static_cast<double>(lead_px) / px_per_ms);
        const std::int64_t loop_shift =
            (obstacle_actions(id) & kActionLoop) != 0 ? kLoopPerfectZoneShiftMs : 0;
        CHECK(window.perfect_close == event.hit_ms + lead_ms + kJudgmentLateBiasMs + loop_shift);
        CHECK(window.perfect_open == window.perfect_close - kPerfectWindowMs * 2);
        CHECK(window.good_close == window.perfect_close + kGoodIntoObstacleMs);
        CHECK(window.good_open == window.perfect_open - (kGoodWindowMs - kPerfectWindowMs));
        if (id == kLoopObstacleId) {
            CHECK(lead_ms < 0);
            CHECK(std::fabs(leading) > 0.1f * width);
            continue;
        }
        CHECK(obstacle_perfect_anchor_x(id, kLoop) ==
              doctest::Approx(obstacle_leading_x(id, kLoop)));
        CHECK(obstacle_leading_x(id, kLoop) == doctest::Approx(0.f).epsilon(0.001));
    }
}

TEST_CASE("the arch is taller than it is wide and the wave is a zig-zag") {
    using namespace oscilline;
    constexpr float kLoop = 96.f;
    const std::vector<Segment> block = obstacle_shape(0, kLoop);
    const float height = -shape_min_y(block);
    const float width = shape_width(block);
    CHECK(height == doctest::Approx(kLoop * kArchWidthFraction * kArchHeightToWidth));
    CHECK(width == doctest::Approx(kLoop * kArchWidthFraction));
    // The arch is a further tenth narrower, keeping the height ratio.
    CHECK(width == doctest::Approx(kLoop * 0.68f * 0.80f * 0.90f));
    CHECK(height == doctest::Approx(kLoop * 0.68f * 0.80f * 0.90f * 1.10f * 1.10f * 1.05f));
    CHECK(kArchHeightToWidth == doctest::Approx(1.10f * 1.10f * 1.05f));
    CHECK(height > width);
    CHECK(kArchHeightToWidth > 1.f);
    bool left_side = false;
    for (const Segment& segment : block) {
        if (std::fabs(segment.x0) < 1.e-3f && std::fabs(segment.x1) < 1.e-3f &&
            std::fabs(segment.y0 - segment.y1) > 1.f) {
            left_side = true;
        }
    }
    CHECK(left_side);
    CHECK(obstacle_leading_x(0, kLoop) == doctest::Approx(0.f).epsilon(0.001));
    CHECK(obstacle_perfect_anchor_x(0, kLoop) ==
          doctest::Approx(obstacle_leading_x(0, kLoop)).epsilon(0.001));
    CHECK(shape_max_y(block) == doctest::Approx(0.f).epsilon(0.001));

    const std::vector<Segment> pit = obstacle_shape(1, kLoop);
    const float pit_width = shape_width(pit);
    const float pit_depth = shape_max_y(pit);
    CHECK(pit_width == doctest::Approx(kLoop * kSpikeHalfWidthFraction * 2.f));
    CHECK(pit_depth == doctest::Approx(kLoop * kSpikeDepthFraction));
    CHECK(pit_width < pit_depth);
    CHECK(shape_min_y(pit) == doctest::Approx(0.f).epsilon(0.001));

    const std::vector<Segment> wave = obstacle_shape(3, kLoop);
    CHECK(shape_width(wave) == doctest::Approx(kLoop * kWaveWidthFraction));
    CHECK(kWaveWidthFraction == doctest::Approx(1.20f * 0.80f * 0.75f));
    CHECK(wave.front().y0 == doctest::Approx(0.f));
    CHECK(wave.back().y1 == doctest::Approx(0.f));
    int above = 0;
    int below = 0;
    count_chain_peaks(wave, 0.f, above, below);
    CHECK(above == kWavePeaksAbove);
    CHECK(below == kWavePeaksBelow);
    const float amplitude = kLoop * kWaveAmplitudeFraction;
    CHECK(shape_min_y(wave) == doctest::Approx(-amplitude));
    CHECK(shape_max_y(wave) == doctest::Approx(amplitude));
    std::vector<float> ys;
    ys.push_back(wave.front().y0);
    for (const Segment& segment : wave) {
        ys.push_back(segment.y1);
    }
    for (std::size_t i = 0; i < ys.size(); ++i) {
        CHECK(ys[i] == doctest::Approx(ys[ys.size() - 1 - i]));
    }
}

// Horizontal center from the samples on the circle's midline. The bottom vertex
// is omitted where the cross opens the circle, so it is not a marker.
float circle_center_x(const std::vector<oscilline::Segment>& shape, float cy, float radius) {
    float left = 1.0e9f;
    float right = -1.0e9f;
    bool any = false;
    const auto take = [&](float x, float y) {
        if (std::fabs(y - cy) > 0.05f) {
            return;
        }
        any = true;
        left = std::min(left, x);
        right = std::max(right, x);
    };
    for (const oscilline::Segment& segment : shape) {
        take(segment.x0, segment.y0);
        take(segment.x1, segment.y1);
    }
    if (!any || right - left < radius) {
        return 0.f;
    }
    return (left + right) * 0.5f;
}

// Each spike tip joins the circle at two points, and the arc between them is absent.
void check_spikes_join_without_chords(const std::vector<oscilline::Segment>& shape,
                                      float cx,
                                      float cy,
                                      float radius) {
    struct Tip {
        float x = 0.f;
        float y = 0.f;
        int arms = 0;
    };
    std::vector<Tip> tips;
    for (const oscilline::Segment& segment : shape) {
        const float d0 = std::hypot(segment.x0 - cx, segment.y0 - cy);
        const float d1 = std::hypot(segment.x1 - cx, segment.y1 - cy);
        const bool out0 = d0 > radius * 1.2f;
        const bool out1 = d1 > radius * 1.2f;
        if (out0 == out1) {
            continue;
        }
        const float x = out0 ? segment.x0 : segment.x1;
        const float y = out0 ? segment.y0 : segment.y1;
        const float bx = out0 ? segment.x1 : segment.x0;
        const float by = out0 ? segment.y1 : segment.y0;
        CHECK(std::fabs(std::hypot(bx - cx, by - cy) - radius) < 0.05f);
        bool found = false;
        for (Tip& tip : tips) {
            if (std::hypot(tip.x - x, tip.y - y) < 1.e-2f) {
                ++tip.arms;
                found = true;
                break;
            }
        }
        if (!found) {
            tips.push_back(Tip{x, y, 1});
        }
    }
    int spikes = 0;
    for (const Tip& tip : tips) {
        if (tip.arms != 2) {
            continue;
        }
        ++spikes;
        const float dir = std::atan2(cy - tip.y, tip.x - cx);
        const float mid_x = cx + std::cos(dir) * radius;
        const float mid_y = cy - std::sin(dir) * radius;
        CHECK(nearest_segment(shape, mid_x, mid_y) > radius * 0.05f);
    }
    CHECK(spikes >= 6);
}

TEST_CASE("pair outlines follow the chart silhouettes") {
    using namespace oscilline;
    constexpr float kLoop = 96.f;
    const float arch_top = -kLoop * kArchWidthFraction * kArchHeightToWidth;
    const float arch_width = kLoop * kArchWidthFraction;
    const auto vertical_side = [](const std::vector<Segment>& shape, float x) {
        for (const Segment& segment : shape) {
            if (std::fabs(segment.x0 - x) < 1.e-3f && std::fabs(segment.x1 - x) < 1.e-3f &&
                std::fabs(segment.y0 - segment.y1) > 1.f) {
                return true;
            }
        }
        return false;
    };

    const std::vector<Segment> block_pit = obstacle_shape(4, kLoop);
    CHECK(vertical_side(block_pit, 0.f));
    CHECK(vertical_side(block_pit, arch_width));
    CHECK(shape_min_y(block_pit) == doctest::Approx(arch_top));
    const float notch_x = arch_width * 0.5f;
    bool notch = false;
    for (const Segment& segment : block_pit) {
        for (const float y : {segment.y0, segment.y1}) {
            if (y > arch_top + 1.f && y < -1.f &&
                (std::fabs(segment.x0 - notch_x) < 1.e-3f ||
                 std::fabs(segment.x1 - notch_x) < 1.e-3f)) {
                notch = true;
            }
        }
    }
    CHECK(notch);
    CHECK(shape_max_y(block_pit) == doctest::Approx(0.f).epsilon(0.001));

    const std::vector<Segment> block_loop = obstacle_shape(5, kLoop);
    const float head = kLoop * kBlockLoopHeadFraction;
    // The circle is as tall as the arch, so it is wider than the arch and
    // becomes the leading edge. The arch stays centered under it.
    const float center = head;
    CHECK(2.f * head == doctest::Approx(-arch_top).epsilon(0.001));
    CHECK(shape_width(block_loop) == doctest::Approx(2.f * head));
    CHECK(vertical_side(block_loop, center - arch_width * 0.5f));
    CHECK(vertical_side(block_loop, center + arch_width * 0.5f));
    CHECK(shape_min_y(block_loop) == doctest::Approx(arch_top - 2.f * head).epsilon(0.01));
    bool head_sits = false;
    for (const Segment& segment : block_loop) {
        head_sits = head_sits || near_point(segment.x0, segment.y0, center, arch_top) ||
                    near_point(segment.x1, segment.y1, center, arch_top);
    }
    CHECK(head_sits);

    const float wave_width = kLoop * kArchWidthFraction * kPairWidthScale;
    const std::vector<Segment> block_wave = obstacle_shape(6, kLoop);
    CHECK(vertical_side(block_wave, 0.f));
    CHECK(vertical_side(block_wave, wave_width));
    int above = 0;
    int below = 0;
    count_chain_peaks(block_wave, arch_top, above, below);
    CHECK(above == kWavePeaksAbove);
    CHECK(below == kWavePeaksBelow);
    CHECK(shape_max_y(block_wave) == doctest::Approx(0.f).epsilon(0.001));

    const float radius = kLoop * kLoopVisualScale * kLoopRadiusFraction;
    const float cy = -kLoopRibbonLift * radius;
    const std::vector<Segment> loop_pit = obstacle_shape(7, kLoop);
    const float pit_cx = circle_center_x(loop_pit, cy, radius);
    CHECK(pit_cx == doctest::Approx(obstacle_leading_x(7, kLoop) + radius));
    check_bottom_cross(loop_pit, pit_cx, cy, radius);
    check_circle_open_above_ribbon(loop_pit, pit_cx, cy, radius);
    CHECK(shape_max_y(loop_pit) == doctest::Approx(0.f).epsilon(0.001));
    const float crown = cy - radius;
    CHECK(shape_min_y(loop_pit) < crown - radius * 0.5f);
    int apex_ends = 0;
    const float apex_y = shape_min_y(loop_pit);
    for (const Segment& segment : loop_pit) {
        if (std::fabs(segment.y0 - apex_y) < 1.e-3f || std::fabs(segment.y1 - apex_y) < 1.e-3f) {
            ++apex_ends;
        }
    }
    CHECK(apex_ends == 2);

    const std::vector<Segment> pit = obstacle_shape(1, kLoop);
    const std::vector<Segment> pit_wave = obstacle_shape(8, kLoop);
    CHECK(shape_max_y(pit_wave) == doctest::Approx(shape_max_y(pit)));
    CHECK(shape_width(pit_wave) > shape_width(pit));
    CHECK(shape_min_y(pit_wave) == doctest::Approx(0.f).epsilon(0.001));
    int tips = 0;
    for (const Segment& segment : pit_wave) {
        if (std::fabs(segment.y0 - shape_max_y(pit_wave)) < 1.e-3f) {
            ++tips;
        }
        if (std::fabs(segment.y1 - shape_max_y(pit_wave)) < 1.e-3f) {
            ++tips;
        }
    }
    CHECK(tips == 2);

    const std::vector<Segment> loop_wave = obstacle_shape(9, kLoop);
    const float wave_cx = circle_center_x(loop_wave, cy, radius);
    CHECK(wave_cx > radius);
    check_bottom_cross(loop_wave, wave_cx, cy, radius);
    check_circle_open_above_ribbon(loop_wave, wave_cx, cy, radius);
    check_spikes_join_without_chords(loop_wave, wave_cx, cy, radius);
    CHECK(shape_max_y(loop_wave) == doctest::Approx(0.f).epsilon(0.001));
    int spikes = 0;
    for (const Segment& segment : loop_wave) {
        const float d0 = std::hypot(segment.x0 - wave_cx, segment.y0 - cy);
        const float d1 = std::hypot(segment.x1 - wave_cx, segment.y1 - cy);
        if (d0 > radius + radius * 0.2f || d1 > radius + radius * 0.2f) {
            ++spikes;
        }
    }
    CHECK(spikes >= 6);
    CHECK(shape_width(loop_wave) > shape_width(obstacle_shape(2, kLoop)));
}

TEST_CASE("a pair total adds both rounds and form carries only when asked") {
    using namespace oscilline;
    RoundScores single;
    single.latest = 300;
    CHECK(pair_total(single) == 300);
    RoundScores paired;
    paired.paired = true;
    paired.earlier = 400;
    paired.latest = 250;
    CHECK(pair_total(paired) == 650);
    RoundJudgment earlier;
    earlier.perfects = 4;
    earlier.goods = 2;
    earlier.misses = 1;
    RoundJudgment latest;
    latest.perfects = 3;
    latest.goods = 1;
    latest.misses = 5;
    const RoundJudgment both = combined_judgment(earlier, latest, true);
    CHECK(both.perfects == 7);
    CHECK(both.goods == 3);
    CHECK(both.misses == 6);
    CHECK(combined_judgment(earlier, latest, false).perfects == 3);
    CHECK(kCarryFormAcrossRounds);

    PlayState previous;
    previous.form = Form::Frog;
    previous.damage = 2;
    previous.clear_run = 3;
    previous.score = 900;
    previous.perfects = 4;
    previous.finished = true;
    previous.event_index = 12;

    PlayState carried;
    carry_form_into(carried, previous, true);
    CHECK(carried.form == Form::Frog);
    CHECK(carried.damage == 2);
    CHECK(carried.clear_run == 3);
    CHECK(carried.score == 0);
    CHECK(carried.perfects == 0);
    CHECK(carried.event_index == 0);
    CHECK_FALSE(carried.finished);

    PlayState fresh;
    carry_form_into(fresh, previous, false);
    CHECK(fresh.form == Form::Rabbit);
    CHECK(fresh.damage == 0);
    CHECK(fresh.clear_run == 0);

    PlayState failed;
    failed.form = Form::Out;
    failed.damage = 3;
    PlayState after_out;
    carry_form_into(after_out, failed, true);
    CHECK(after_out.form == Form::Rabbit);
    CHECK(after_out.damage == 0);
}

TEST_CASE("a close pair scales down and a wide gap does not") {
    using namespace oscilline;
    constexpr float kLoop = 96.f;
    const float width = obstacle_width(9, kLoop);
    CHECK(width == doctest::Approx(shape_width(obstacle_shape(9, kLoop))));
    CHECK(width > kLoop);
    CHECK(pair_fit_scale(width, width + 10.f) == doctest::Approx(1.f));
    const float tight = width * 0.5f;
    const float scale = pair_fit_scale(width, tight);
    CHECK(scale < 1.f);
    CHECK(scale == doctest::Approx(tight * kPairFitMargin / width));
    CHECK(width * scale <= tight);
    CHECK(pair_fit_scale(width, 0.f) == doctest::Approx(kMinPairFitScale));
    CHECK(pair_fit_scale(0.f, 10.f) == doctest::Approx(1.f));
    CHECK(obstacle_is_pair(9));
    CHECK(obstacle_is_pair(4));
    CHECK_FALSE(obstacle_is_pair(2));
    CHECK(obstacle_width(10, kLoop) == doctest::Approx(0.f));
}

TEST_CASE("the difficulty wheel wraps and each row maps to a course pair") {
    using namespace oscilline;
    CHECK(wheel_move(0, -1) == kWheelSlots - 1);
    CHECK(wheel_move(kWheelSlots - 1, 1) == 0);
    CHECK(wheel_move(1, 1) == 2);
    CHECK(wheel_move(2, -3) == kWheelSlots - 1);

    const auto bronze = wheel_difficulty(0);
    const auto silver = wheel_difficulty(1);
    const auto gold = wheel_difficulty(2);
    REQUIRE(bronze.has_value());
    REQUIRE(silver.has_value());
    REQUIRE(gold.has_value());
    CHECK(bronze.value() == Difficulty::Bronze);
    CHECK(silver.value() == Difficulty::Silver);
    CHECK(gold.value() == Difficulty::Gold);
    CHECK(courses_for(bronze.value()).first == 0);
    CHECK(courses_for(bronze.value()).second == 1);
    CHECK(courses_for(silver.value()).first == 2);
    CHECK(courses_for(silver.value()).second == 3);
    CHECK(courses_for(gold.value()).first == 4);
    CHECK(courses_for(gold.value()).second == 5);
    CHECK(difficulty_name(Difficulty::Bronze) == "bronze");
    CHECK(difficulty_name(Difficulty::Silver) == "silver");
    CHECK(difficulty_name(Difficulty::Gold) == "gold");
    CHECK_FALSE(wheel_difficulty(kWheelScores).has_value());
    CHECK_FALSE(wheel_difficulty(kWheelBack).has_value());
    CHECK_FALSE(wheel_difficulty(-1).has_value());

    ScoreBoard board;
    note_score(board, Difficulty::Gold, 400);
    note_score(board, Difficulty::Gold, 150);
    CHECK(board.best[static_cast<int>(Difficulty::Gold)] == 400);
    note_score(board, Difficulty::Gold, 900);
    CHECK(board.best[static_cast<int>(Difficulty::Gold)] == 900);
    CHECK(board.best[static_cast<int>(Difficulty::Bronze)] == 0);

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto file = std::filesystem::temp_directory_path() /
                      ("oscilline-course-scores-" + std::to_string(stamp) + ".txt");
    REQUIRE(save_high_scores(file, board));
    const auto loaded = load_high_scores(file);
    REQUIRE(loaded);
    CHECK(loaded.value().best[static_cast<int>(Difficulty::Gold)] == 900);
    CHECK(loaded.value().best[static_cast<int>(Difficulty::Bronze)] == 0);

    const ScoreBoard cleared;
    REQUIRE(save_high_scores(file, cleared));
    const auto after_reset = load_high_scores(file);
    REQUIRE(after_reset);
    CHECK(after_reset.value().best[static_cast<int>(Difficulty::Gold)] == 0);
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

TEST_CASE("the prelude camera reaches play framing at track zero without moving the outro") {
    using namespace oscilline;
    constexpr std::int32_t end = 20000;
    CHECK(kCourseStartDelayMs == 8000);
    CHECK(camera_at(-kCourseStartDelayMs, end, 640.f, true).phase == CameraPhase::Intro);
    CHECK(camera_at(-1, end, 640.f, true).phase == CameraPhase::Intro);
    const auto start = camera_at(0, end, 640.f, true);
    CHECK(start.phase == CameraPhase::Play);
    CHECK(start.figure_x == doctest::Approx(640.f * kHitXFraction));
    CHECK(camera_at(end - 1, end, 640.f, true).phase == CameraPhase::Play);
    CHECK(camera_at(end, end, 640.f, true).phase == CameraPhase::Outro);
    CHECK(camera_at(end + kCameraOutroMs, end, 640.f, true).figure_x ==
          doctest::Approx(640.f * kCameraIntroCenterFraction));
}

// Locked hash of build_course for synthetic courses 1–6. A mapping change that
// moves a hit, a type, or an approach time changes this value.
constexpr std::uint64_t kPlayableCourseHash = 16122142268959959684ull;

TEST_CASE("disc courses 1 through 6 stay byte-identical") {
    using namespace oscilline;
    CHECK(kPlayableCourses == 6);

    Words words;
    constexpr std::uint32_t kTracks = 7;
    header(words, kTracks, kTracks, kTracks, kTracks, 1, 0);
    words.u32(4);
    words.u32(0);
    words.u32(1);
    words.u32(2);
    words.u32(3);
    for (std::uint32_t track = 0; track < kTracks; ++track) {
        words.u32(1);
        words.i32(0);
        words.i32(4410);
        words.i32(22050);
        words.i32(0);
        words.i32(0);
    }
    for (std::uint32_t track = 0; track < kTracks; ++track) {
        words.u32(1);
        words.i32(22050 * static_cast<std::int32_t>(2 + track));
        words.i32(0);
        words.i32(1);
        words.i32(0);
    }
    for (std::uint32_t track = 0; track < kTracks; ++track) {
        words.u32(0);
    }
    for (std::uint32_t track = 0; track < kTracks; ++track) {
        words.u32(track);
        words.u32(track);
        words.u32(track);
    }

    const auto file = parse_fsl(words.bytes);
    REQUIRE(file);
    REQUIRE(file.value().track_index.size() == kTracks);
    const auto progression = build_course(file.value(), 6, 20000);
    REQUIRE(progression);

    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (int track = 0; track < kPlayableCourses; ++track) {
        const auto timeline = must_course(file.value(), track, 20000);
        mix(static_cast<std::uint64_t>(timeline.cdda_track));
        mix(static_cast<std::uint32_t>(timeline.duration_ms));
        mix(timeline.events.size());
        for (const auto& event : timeline.events) {
            mix(event.obstacle);
            mix(static_cast<std::uint32_t>(event.hit_ms));
            mix(static_cast<std::uint32_t>(event.approach_ms));
            mix(static_cast<std::uint32_t>(event.scroll_approach_ms));
        }
    }
    INFO("playable course hash " << hash);
    CHECK(hash == kPlayableCourseHash);
}

TEST_CASE("eighteen clears promote worm, frog, and rabbit, and a miss clears the streak") {
    using namespace oscilline;
    CHECK(kClearsToRise == 18);
    CHECK(form_name(Form::Super) == "super");
    CHECK(form_miss_limit(Form::Super) == kRabbitMisses);
    CHECK(form_score_multiplier(Form::Super) == kSuperScoreMultiplier);
    CHECK(kSuperScoreMultiplier == 2);

    CourseTimeline timeline = hits_at(kClearsToRise * 3 + 1, 500, 60000);
    PlayState state;
    state.form = Form::Worm;
    int cursor = 0;
    std::vector<PlayHit> hits;
    const auto clear_n = [&](int count) {
        for (int i = 0; i < count; ++i) {
            const std::int64_t when = perfect_at(timeline.events[static_cast<std::size_t>(cursor)]);
            hits.clear();
            play_advance(state, timeline, when - 10, when, kActionBlock, &hits);
            ++cursor;
        }
    };

    clear_n(kClearsToRise - 1);
    CHECK(state.form == Form::Worm);
    CHECK(state.clear_run == kClearsToRise - 1);
    clear_n(1);
    CHECK(state.form == Form::Frog);
    CHECK(state.clear_run == 0);
    CHECK_FALSE(hits.back().super_transform);
    CHECK(state.burst_until_ms == 0);
    CHECK_FALSE(state.super_transform.active_at(
        timeline.events[static_cast<std::size_t>(cursor - 1)].hit_ms));
    CHECK(state.score == 179);

    clear_n(kClearsToRise);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.clear_run == 0);
    CHECK_FALSE(hits.back().super_transform);
    CHECK(state.burst_until_ms == 0);

    clear_n(kClearsToRise - 1);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.clear_run == kClearsToRise - 1);
    clear_n(1);
    CHECK(state.form == Form::Super);
    CHECK(state.clear_run == 0);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.back().form_changed);
    CHECK(hits.back().super_transform);
    const std::int64_t promoted_at =
        perfect_at(timeline.events[static_cast<std::size_t>(cursor - 1)]);
    CHECK(state.burst_until_ms == 0);
    CHECK(state.super_transform.start_ms == promoted_at);
    CHECK(state.super_transform.until_ms == promoted_at + kSuperTransformMs);
    CHECK(state.super_transform.active_at(promoted_at));
    CHECK_FALSE(state.super_transform.active_at(promoted_at + kSuperTransformMs));
    CHECK(state.super_transform.sfx_id == kSuperTransformSfxId);
    CHECK(kSuperTransformSfxId == static_cast<int>(SfxId::SuperPromote));
    CHECK(state.super_transform.clip_id == kSuperTransformClipId);
    CHECK(kSuperTransformClipId < 0);
    CHECK(state.score == 647);

    clear_n(1);
    CHECK(state.form == Form::Super);
    CHECK(state.clear_run == 1);
    CHECK(state.score == 671);

    PlayState streak;
    streak.form = Form::Rabbit;
    CourseTimeline short_run = hits_at(4, 500, 20000);
    for (int i = 0; i < 3; ++i) {
        const std::int32_t hit = short_run.events[static_cast<std::size_t>(i)].hit_ms;
        play_advance(streak, short_run, hit - 10, hit, kActionBlock);
    }
    CHECK(streak.clear_run == 3);
    CHECK(streak.form == Form::Rabbit);
    const ObstacleWindow miss_window = obstacle_window(short_run.events[3]);
    play_advance(streak, short_run, miss_window.good_open, miss_window.good_close + 1, 0);
    CHECK(streak.last == Judgment::Miss);
    CHECK(streak.clear_run == 0);
    CHECK(streak.form == Form::Rabbit);
    CHECK(streak.damage == 1);
}

TEST_CASE("super drops to rabbit after the rabbit miss limit") {
    using namespace oscilline;
    CourseTimeline timeline = hits_at(kRabbitMisses, 500, 30000);
    PlayState state;
    state.form = Form::Super;
    state.clear_run = 5;
    state.super_transform.start_ms = 10;
    state.super_transform.until_ms = 10 + kSuperTransformMs;
    for (int i = 0; i < kRabbitMisses - 1; ++i) {
        const ObstacleWindow window = obstacle_window(timeline.events[static_cast<std::size_t>(i)]);
        play_advance(state, timeline, window.good_open, window.good_close + 1, 0);
    }
    CHECK(state.form == Form::Super);
    CHECK(state.damage == kRabbitMisses - 1);
    CHECK(state.clear_run == 0);
    const ObstacleWindow last_window =
        obstacle_window(timeline.events[static_cast<std::size_t>(kRabbitMisses - 1)]);
    const std::int64_t when = last_window.good_close + 1;
    play_advance(state, timeline, last_window.good_open, when, 0);
    CHECK(state.form == Form::Rabbit);
    CHECK(state.damage == 0);
    CHECK(state.burst_until_ms == when + kFormBurstMs);
    CHECK(state.super_transform.until_ms == 0);
    CHECK_FALSE(state.finished);

    PlayState carried;
    carry_form_into(carried, state, true);
    CHECK(carried.form == Form::Rabbit);
    PlayState super;
    super.form = Form::Super;
    super.damage = 2;
    super.clear_run = 7;
    super.score = 400;
    PlayState next;
    carry_form_into(next, super, true);
    CHECK(next.form == Form::Super);
    CHECK(next.damage == 2);
    CHECK(next.clear_run == 7);
    CHECK(next.score == 0);
    CHECK(next.super_transform.until_ms == 0);
}

TEST_CASE("streak ring dots sit in their slots") {
    using namespace oscilline;
    constexpr float kFigureX = 100.f;
    constexpr float kRibbonY = 240.f;
    const float height = static_cast<float>(kLogicalHeight);
    const float cx = kFigureX;
    const float cy = kRibbonY - kStreakCenterAboveRibbon * height;
    const float rx = kStreakRadiusXFraction * height;
    const float ry = kStreakRadiusYFraction * height;
    CHECK(kStreakSlotCount == 17);
    CHECK(kStreakStepDeg == doctest::Approx(360.f / 17.f));
    CHECK(rx == doctest::Approx(0.133f * height));
    CHECK(ry == doctest::Approx(0.145f * height));
    CHECK(kStreakDotR == 208);
    CHECK(kStreakDotG == 88);
    CHECK(kStreakDotB == 176);

    const StreakDot top = streak_ring_dot(kFigureX, kRibbonY, height, 0);
    CHECK(top.x == doctest::Approx(cx).epsilon(0.001));
    CHECK(top.y == doctest::Approx(cy - ry).epsilon(0.001));

    const StreakDot next = streak_ring_dot(kFigureX, kRibbonY, height, 1);
    CHECK(next.x < cx);
    CHECK(next.y < cy);
    const float step = kStreakStepDeg * 3.14159265f / 180.f;
    CHECK(next.x == doctest::Approx(cx + rx * std::cos(3.14159265f / 2.f + step)).epsilon(0.001));
    CHECK(next.y == doctest::Approx(cy - ry * std::sin(3.14159265f / 2.f + step)).epsilon(0.001));

    const StreakDot fourth = streak_ring_dot(kFigureX, kRibbonY, height, 4);
    const float fourth_angle = 3.14159265f / 2.f + 4.f * step;
    CHECK(fourth.x == doctest::Approx(cx + rx * std::cos(fourth_angle)).epsilon(0.001));
    CHECK(fourth.y == doctest::Approx(cy - ry * std::sin(fourth_angle)).epsilon(0.001));

    CHECK(streak_ring_dots(Form::Rabbit, 1) == 1);
    CHECK(streak_ring_dots(Form::Rabbit, 17) == 17);
    CHECK(streak_ring_dots(Form::Rabbit, 18) == 17);
    CHECK(streak_ring_dots(Form::Super, 17) == 0);
    CHECK(streak_ring_dots(Form::Super, 1) == 0);
    CHECK(streak_ring_dots(Form::Frog, 0) == 0);
}

TEST_CASE("the super burst dashes grow on the slot rays and end together") {
    using namespace oscilline;
    constexpr float kCenterX = 200.f;
    constexpr float kCenterY = 180.f;
    const float height = static_cast<float>(kLogicalHeight);
    CHECK(kSuperTransformMs == 280);
    CHECK(kSuperDashR == 232);
    CHECK(kSuperDashG == 170);
    CHECK(kSuperDashB == 214);
    CHECK(kSuperBurstGrowth == doctest::Approx(3.8f));
    CHECK(kSuperDashLengthFraction == doctest::Approx(0.16f));

    const float ry = kStreakRadiusYFraction * height;
    const SuperDash start = super_burst_dash(kCenterX, kCenterY, height, 0, 0.f);
    CHECK(start.radius == doctest::Approx(ry));
    CHECK(start.length == doctest::Approx(0.16f * ry));
    CHECK(start.x0 == doctest::Approx(kCenterX).epsilon(0.001));
    CHECK(start.x1 == doctest::Approx(kCenterX).epsilon(0.001));
    CHECK(start.y1 ==
          doctest::Approx(kCenterY - (start.radius + 0.5f * start.length)).epsilon(0.001));
    CHECK(start.y0 ==
          doctest::Approx(kCenterY - (start.radius - 0.5f * start.length)).epsilon(0.001));
    CHECK(start.length / height == doctest::Approx(0.16f * 0.145f).epsilon(0.001));

    const SuperDash end = super_burst_dash(kCenterX, kCenterY, height, 0, 1.f);
    CHECK(end.radius == doctest::Approx(ry * (1.f + 3.8f)));
    CHECK(end.length == doctest::Approx(0.16f * end.radius));
    CHECK(end.length / height == doctest::Approx(0.16f * 4.8f * 0.145f).epsilon(0.001));
    CHECK(end.y1 < start.y1);

    const float u = 0.5f;
    const float grown = 1.f - (1.f - u) * (1.f - u);
    const SuperDash mid = super_burst_dash(kCenterX, kCenterY, height, 3, u);
    const float r0 =
        std::hypot(kStreakRadiusXFraction * height *
                       std::cos(3.14159265f / 2.f + 3.f * kStreakStepDeg * 3.14159265f / 180.f),
                   kStreakRadiusYFraction * height *
                       std::sin(3.14159265f / 2.f + 3.f * kStreakStepDeg * 3.14159265f / 180.f));
    CHECK(mid.radius == doctest::Approx(r0 * (1.f + 3.8f * grown)).epsilon(0.001));
    CHECK(super_burst_u(1000, 1000) == doctest::Approx(0.f));
    CHECK(super_burst_u(1000, 1000 + kSuperTransformMs) == doctest::Approx(1.f));

    SuperTransform effect;
    effect.start_ms = 1000;
    effect.until_ms = 1000 + kSuperTransformMs;
    CHECK(effect.active_at(1000));
    CHECK_FALSE(effect.active_at(1000 + kSuperTransformMs));
}

TEST_CASE("build_course keeps pattern spans and marks gold courses") {
    Chart chart;
    chart.fixed.push_back({{0}});
    chart.control.push_back({});
    chart.courses = 6;
    chart.patterns.push_back({0, 1, 0});
    chart.patterns.push_back({10 * 22050, 0, 0});
    chart.patterns.push_back({18 * 22050, 1, 0});
    chart.patterns.push_back({28 * 22050, 0, 0});
    chart.patterns.push_back({36 * 22050, 1, 0});
    const oscilline::FslFile file = must_fsl(chart);
    const oscilline::CourseTimeline bronze = must_course(file, 0, 50000);
    REQUIRE(bronze.camera_sections.size() == 3);
    CHECK(bronze.camera_sections[0].start_ms == 0);
    CHECK(bronze.camera_sections[0].end_ms == 10000);
    CHECK(bronze.camera_sections[1].start_ms == 18000);
    CHECK(bronze.camera_sections[2].end_ms == 50000);
    CHECK_FALSE(bronze.gold_shift);
    CHECK_FALSE(must_course(file, 3, 50000).gold_shift);
    CHECK(must_course(file, 4, 50000).gold_shift);
    CHECK(must_course(file, 5, 50000).gold_shift);
    CHECK(must_course(file, 4, 50000).camera_sections.size() == 3);
}
