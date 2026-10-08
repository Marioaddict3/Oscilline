// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Section stretch, transitions, guesses, and the Gold shift.

#include "oscilline/course/camera.hpp"
#include "oscilline/course/camera_schedule.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/course/music.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <vector>

namespace {

std::int64_t sec_ms(double seconds) {
    return static_cast<std::int64_t>(std::llround(seconds * 1000.0));
}

oscilline::CameraKeyCounts observed_keys() {
    oscilline::CameraKeyCounts keys;
    keys.s01 = oscilline::kObservedS01Keys;
    keys.s02 = oscilline::kObservedS02Keys;
    keys.b01 = oscilline::kObservedB01Keys;
    keys.tv_ss = oscilline::kObservedTransitionKeys;
    keys.tv_bb = oscilline::kObservedTransitionKeys;
    keys.tv_bs = oscilline::kObservedTransitionKeys;
    return keys;
}

// PAL audio-head windows for courses 1–4. Section ends are the in-play spans.
// The third section's end was not timed; 160 s is only a stand-in so the
// stretch has a finite span.
std::vector<oscilline::CameraSectionSpan> course1_sections() {
    return {
        {sec_ms(8.42), sec_ms(48.29)},
        {sec_ms(62.04), sec_ms(101.78)},
        {sec_ms(120.71), sec_ms(160.0)},
    };
}

std::vector<oscilline::CameraSectionSpan> course2_sections() {
    return {
        {sec_ms(8.14), sec_ms(47.83)},
        {sec_ms(69.21), sec_ms(109.68)},
        {sec_ms(118.09), sec_ms(160.0)},
    };
}

std::vector<oscilline::CameraSectionSpan> course4_sections() {
    return {
        {sec_ms(8.15), sec_ms(48.63)},
        {sec_ms(57.03), sec_ms(96.76)},
        {sec_ms(119.67), sec_ms(160.0)},
    };
}

} // namespace

TEST_CASE("the in-play camera stretches its keys across the section") {
    using namespace oscilline;
    const auto sections = course1_sections();
    const CameraKeyCounts keys = observed_keys();
    const CameraSectionSpan& first = sections[0];

    const CameraCue at_start = schedule_disc_camera(0, first.start_ms, sections, keys);
    CHECK(at_start.kind == CameraCueKind::Play);
    CHECK(at_start.road == RoadCamera::S01);
    CHECK(at_start.key_index == doctest::Approx(0.f));

    const CameraCue at_end = schedule_disc_camera(0, first.end_ms, sections, keys);
    CHECK(at_end.kind == CameraCueKind::Play);
    CHECK(at_end.key_index == doctest::Approx(static_cast<float>(kObservedS01Keys - 1)));

    const std::int64_t mid = first.start_ms + (first.end_ms - first.start_ms) / 2;
    const CameraCue halfway = schedule_disc_camera(0, mid, sections, keys);
    CHECK(halfway.key_index == doctest::Approx(12.5f));
    CHECK(stretched_key_index(mid, first.start_ms, first.end_ms, kObservedS01Keys) ==
          doctest::Approx(12.5f));

    const auto course2 = course2_sections();
    const CameraCue s02_start = schedule_disc_camera(1, course2[1].start_ms, course2, keys);
    CHECK(s02_start.kind == CameraCueKind::Play);
    CHECK(s02_start.road == RoadCamera::S02);
    CHECK(s02_start.key_index == doctest::Approx(0.f));
    const CameraCue s02_end = schedule_disc_camera(1, course2[1].end_ms, course2, keys);
    CHECK(s02_end.road == RoadCamera::S02);
    CHECK(s02_end.key_index == doctest::Approx(static_cast<float>(kObservedS02Keys - 1)));
}

TEST_CASE("the camera holds the last key until the transition") {
    using namespace oscilline;
    const auto sections = course1_sections();
    const CameraKeyCounts keys = observed_keys();
    const std::int64_t duration = transition_duration_ms(kObservedTransitionKeys);
    CHECK(duration == 8000);
    const std::int64_t transition_start = sections[1].start_ms - duration;
    // 48.29 s is the section end. The 30 keys/s transition into 62.04 s starts
    // 8.0 s earlier, so the hold is the gap before that.
    REQUIRE(sections[0].end_ms < transition_start);
    REQUIRE(transition_start < sections[1].start_ms);

    const CameraCue held = schedule_disc_camera(0, sections[0].end_ms + 1000, sections, keys);
    CHECK(held.kind == CameraCueKind::Hold);
    CHECK(held.section == 0);
    CHECK(held.road == RoadCamera::S01);
    CHECK(held.key_index == doctest::Approx(static_cast<float>(kObservedS01Keys - 1)));

    const CameraCue still = schedule_disc_camera(0, transition_start - 1, sections, keys);
    CHECK(still.kind == CameraCueKind::Hold);
    CHECK(still.key_index == held.key_index);
}

TEST_CASE("a transition ends exactly when the next section starts") {
    using namespace oscilline;
    const auto sections = course1_sections();
    const CameraKeyCounts keys = observed_keys();
    const std::int64_t duration = transition_duration_ms(kObservedTransitionKeys);
    const std::int64_t next = sections[1].start_ms;
    CHECK(next - duration + duration == next);

    const CameraCue opening = schedule_disc_camera(0, next - duration, sections, keys);
    CHECK(opening.kind == CameraCueKind::Transition);
    CHECK(opening.playing);
    CHECK(opening.transition == TransitionCamera::TvSs);
    CHECK(opening.section == 1);
    CHECK(opening.key_index == doctest::Approx(0.f));

    const CameraCue almost = schedule_disc_camera(0, next - 1, sections, keys);
    CHECK(almost.kind == CameraCueKind::Transition);
    CHECK(almost.playing);
    CHECK(almost.key_index == doctest::Approx(239.97f).epsilon(0.0001));

    const CameraCue handed = schedule_disc_camera(0, next, sections, keys);
    CHECK(handed.kind == CameraCueKind::Play);
    CHECK(handed.section == 1);
    CHECK(handed.road == RoadCamera::S01);
    CHECK(handed.key_index == doctest::Approx(0.f));

    // The intro is the same transition, and it ends at section 1.
    const std::int64_t intro_start = sections[0].start_ms - duration;
    const CameraCue before = schedule_disc_camera(0, intro_start - 1, sections, keys);
    CHECK(before.kind == CameraCueKind::Transition);
    CHECK_FALSE(before.playing);
    CHECK(before.key_index == doctest::Approx(0.f));
    CHECK(before.transition == TransitionCamera::TvSs);

    const CameraCue intro = schedule_disc_camera(0, intro_start, sections, keys);
    CHECK(intro.playing);
    CHECK(intro.transition == TransitionCamera::TvSs);
    CHECK(intro.key_index == doctest::Approx(0.f));

    const CameraCue intro_done = schedule_disc_camera(0, sections[0].start_ms, sections, keys);
    CHECK(intro_done.kind == CameraCueKind::Play);
    CHECK(intro_done.key_index == doctest::Approx(0.f));
}

TEST_CASE("section cameras and transitions follow the observed table") {
    using namespace oscilline;
    CHECK(course_section_camera(0, 0) == RoadCamera::S01);
    CHECK(course_section_camera(0, 1) == RoadCamera::S01);
    CHECK(course_section_camera(0, 2) == RoadCamera::S01);
    CHECK(course_section_camera(1, 0) == RoadCamera::S01);
    CHECK(course_section_camera(1, 1) == RoadCamera::S02);
    CHECK(course_section_camera(1, 2) == RoadCamera::S01);
    CHECK(course_section_camera(2, 0) == RoadCamera::S02);
    CHECK(course_section_camera(2, 1) == RoadCamera::S01);
    CHECK(course_section_camera(2, 2) == RoadCamera::S02);
    CHECK(course_section_camera(3, 0) == RoadCamera::B01);
    CHECK(course_section_camera(3, 1) == RoadCamera::B01);
    CHECK(course_section_camera(3, 2) == RoadCamera::B01);

    CHECK_FALSE(course_section_camera_is_guess(0, 0));
    CHECK_FALSE(course_section_camera_is_guess(3, 2));
    // Course 5: S02 was observed. B01 and the closing S02 were not. TV_BS into
    // section 2 is the mix those guesses produce. Course 6 is a placeholder.
    CHECK(course_section_camera(4, 0) == RoadCamera::S02);
    CHECK_FALSE(course_section_camera_is_guess(4, 0));
    CHECK(course_section_camera(4, 1) == RoadCamera::B01);
    CHECK(course_section_camera_is_guess(4, 1));
    CHECK(course_section_camera(4, 2) == RoadCamera::S02);
    CHECK(course_section_camera_is_guess(4, 2));
    CHECK(course_section_camera(5, 0) == RoadCamera::S02);
    CHECK(course_section_camera(5, 1) == RoadCamera::S01);
    CHECK(course_section_camera(5, 2) == RoadCamera::S02);
    CHECK(course_section_camera_is_guess(5, 0));
    CHECK(course_section_camera_is_guess(5, 1));
    CHECK(course_section_camera_is_guess(5, 2));

    CHECK(transition_into_section(0, 0) == TransitionCamera::TvSs);
    CHECK(transition_into_section(1, 1) == TransitionCamera::TvSs);
    CHECK(transition_into_section(3, 0) == TransitionCamera::TvBb);
    CHECK(transition_into_section(3, 1) == TransitionCamera::TvBb);
    CHECK(transition_into_section(4, 0) == TransitionCamera::TvSs);
    CHECK(transition_into_section(4, 1) == TransitionCamera::TvBs);
    CHECK(transition_into_section(4, 2) == TransitionCamera::TvBs);

    const auto course4 = course4_sections();
    const CameraKeyCounts keys = observed_keys();
    const std::int64_t intro_at = course4[0].start_ms - 1000;
    const CameraCue intro = schedule_disc_camera(3, intro_at, course4, keys);
    CHECK(intro.kind == CameraCueKind::Transition);
    CHECK(intro.transition == TransitionCamera::TvBb);
    CHECK(intro.playing);

    // TV_BS was observed to start as course 5 section 1 ended (48.47 s). That
    // is the no-hold case: the next section starts one transition later.
    const std::int64_t ended = sec_ms(48.47);
    const std::int64_t duration = transition_duration_ms(kObservedTransitionKeys);
    const std::vector<CameraSectionSpan> course5 = {
        {sec_ms(8.43), ended},
        {ended + duration, ended + duration + sec_ms(40.0)},
        {ended + duration + sec_ms(50.0), ended + duration + sec_ms(90.0)},
    };
    const CameraCue mix = schedule_disc_camera(4, ended, course5, keys);
    CHECK(mix.kind == CameraCueKind::Transition);
    CHECK(mix.transition == TransitionCamera::TvBs);
    CHECK(mix.playing);
    CHECK(mix.key_index == doctest::Approx(0.f));
    const CameraCue mix_done = schedule_disc_camera(4, course5[1].start_ms, course5, keys);
    CHECK(mix_done.kind == CameraCueKind::Play);
    CHECK(mix_done.road == RoadCamera::B01);
    CHECK(mix_done.key_index == doctest::Approx(0.f));
}

TEST_CASE("a missing road file falls back to held S01") {
    using namespace oscilline;
    const auto sections = course2_sections();
    CameraKeyCounts keys = observed_keys();
    keys.s02 = 0;
    const std::int64_t mid = sections[1].start_ms + (sections[1].end_ms - sections[1].start_ms) / 2;
    const CameraCue missing = schedule_disc_camera(1, mid, sections, keys);
    CHECK(missing.kind == CameraCueKind::FallbackS01);
    CHECK(missing.file_missing);
    CHECK(missing.key_index == doctest::Approx(0.f));

    keys.tv_ss = 0;
    const CameraCue no_intro = schedule_disc_camera(1, 0, sections, keys);
    CHECK(no_intro.kind == CameraCueKind::FallbackS01);
    CHECK(no_intro.file_missing);
}

TEST_CASE("pattern segments become camera section spans") {
    using namespace oscilline;
    std::vector<FslPatternSegment> segments(5);
    segments[0].start_time = 0;
    segments[0].pattern_type = 1;
    segments[1].start_time = 10 * 22050;
    segments[1].pattern_type = 0;
    segments[2].start_time = 18 * 22050;
    segments[2].pattern_type = 1;
    segments[3].start_time = 28 * 22050;
    segments[3].pattern_type = 0;
    segments[4].start_time = 36 * 22050;
    segments[4].pattern_type = 1;

    const std::vector<CameraSectionSpan> spans =
        camera_sections_from_patterns(segments, 50000, kDefaultUnitsPerSecond);
    REQUIRE(spans.size() == 3);
    CHECK(spans[0].start_ms == 0);
    CHECK(spans[0].end_ms == 10000);
    CHECK(spans[1].start_ms == 18000);
    CHECK(spans[1].end_ms == 28000);
    CHECK(spans[2].start_ms == 36000);
    CHECK(spans[2].end_ms == 50000);

    std::vector<FslPatternSegment> swapped = segments;
    std::swap(swapped[0], swapped[4]);
    const std::vector<CameraSectionSpan> ordered =
        camera_sections_from_patterns(swapped, 50000, kDefaultUnitsPerSecond);
    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0].start_ms == spans[0].start_ms);
    CHECK(ordered[2].end_ms == spans[2].end_ms);

    std::vector<FslPatternSegment> only_last(1);
    only_last[0].start_time = 0;
    only_last[0].pattern_type = 2;
    const std::vector<CameraSectionSpan> tail =
        camera_sections_from_patterns(only_last, 0, kDefaultUnitsPerSecond);
    REQUIRE(tail.size() == 1);
    CHECK(tail[0].start_ms == 0);
    CHECK(tail[0].end_ms == kTailMsWithoutAudio);
}

TEST_CASE("custom music splits the track and selects the tier camera") {
    using namespace oscilline;
    CHECK(custom_music_camera_sections(0).empty());
    CHECK(custom_music_camera_sections(-1).empty());

    const std::vector<CameraSectionSpan> three = custom_music_camera_sections(90000);
    REQUIRE(three.size() == 3);
    CHECK(three[0].start_ms == 0);
    CHECK(three[0].end_ms == 30000);
    CHECK(three[1].start_ms == 30000);
    CHECK(three[1].end_ms == 60000);
    CHECK(three[2].start_ms == 60000);
    CHECK(three[2].end_ms == 90000);

    const std::vector<CameraSectionSpan> exact = custom_music_camera_sections(60000);
    REQUIRE(exact.size() == 3);
    CHECK(exact[0].end_ms - exact[0].start_ms == kCustomMusicSectionMinMs);
    CHECK(exact[2].end_ms == 60000);

    const std::vector<CameraSectionSpan> leftover = custom_music_camera_sections(60001);
    REQUIRE(leftover.size() == 3);
    CHECK(leftover[0].end_ms - leftover[0].start_ms >= kCustomMusicSectionMinMs);
    CHECK(leftover[2].end_ms == 60001);
    CHECK(leftover[2].end_ms - leftover[0].start_ms == 60001);

    const std::vector<CameraSectionSpan> two = custom_music_camera_sections(50000);
    REQUIRE(two.size() == 2);
    CHECK(two[0].start_ms == 0);
    CHECK(two[0].end_ms == 25000);
    CHECK(two[1].start_ms == 25000);
    CHECK(two[1].end_ms == 50000);

    const std::vector<CameraSectionSpan> one = custom_music_camera_sections(30000);
    REQUIRE(one.size() == 1);
    CHECK(one[0].start_ms == 0);
    CHECK(one[0].end_ms == 30000);
    REQUIRE(custom_music_camera_sections(kCustomMusicSectionMinMs).size() == 1);

    CHECK(custom_music_camera_course(Difficulty::Bronze) == 0);
    CHECK(custom_music_camera_course(Difficulty::Silver) == 2);
    CHECK(custom_music_camera_course(Difficulty::Gold) == 4);
    CHECK_FALSE(custom_music_gold_shift(Difficulty::Bronze));
    CHECK_FALSE(custom_music_gold_shift(Difficulty::Silver));
    CHECK(custom_music_gold_shift(Difficulty::Gold));

    const CameraKeyCounts keys = observed_keys();
    const std::int64_t intro_ms = transition_duration_ms(keys.tv_ss);
    const int bronze = custom_music_camera_course(Difficulty::Bronze);
    const CameraCue bronze_intro = schedule_disc_camera(bronze, -intro_ms, three, keys);
    CHECK(bronze_intro.kind == CameraCueKind::Transition);
    CHECK(bronze_intro.transition == TransitionCamera::TvSs);
    CHECK(bronze_intro.section == 0);
    CHECK(bronze_intro.playing);
    const CameraCue bronze_play = schedule_disc_camera(bronze, 1000, three, keys);
    CHECK(bronze_play.kind == CameraCueKind::Play);
    CHECK(bronze_play.road == RoadCamera::S01);
    CHECK(bronze_play.section == 0);
    const CameraCue bronze_next = schedule_disc_camera(bronze, three[1].start_ms - 1, three, keys);
    CHECK(bronze_next.kind == CameraCueKind::Transition);
    CHECK(bronze_next.transition == TransitionCamera::TvSs);
    CHECK(bronze_next.section == 1);
    const CameraCue bronze_mid =
        schedule_disc_camera(bronze, three[1].start_ms + 1000, three, keys);
    CHECK(bronze_mid.kind == CameraCueKind::Play);
    CHECK(bronze_mid.road == RoadCamera::S01);
    CHECK(bronze_mid.section == 1);

    const int silver = custom_music_camera_course(Difficulty::Silver);
    const CameraCue silver_play = schedule_disc_camera(silver, 1000, three, keys);
    CHECK(silver_play.kind == CameraCueKind::Play);
    CHECK(silver_play.road == RoadCamera::S02);
    const CameraCue silver_mid =
        schedule_disc_camera(silver, three[1].start_ms + 1000, three, keys);
    CHECK(silver_mid.road == RoadCamera::S01);
    CHECK(silver_mid.kind == CameraCueKind::Play);
    const CameraCue silver_last =
        schedule_disc_camera(silver, three[2].start_ms + 1000, three, keys);
    CHECK(silver_last.road == RoadCamera::S02);

    const int gold = custom_music_camera_course(Difficulty::Gold);
    const CameraCue gold_intro = schedule_disc_camera(gold, -intro_ms, three, keys);
    CHECK(gold_intro.kind == CameraCueKind::Transition);
    CHECK(gold_intro.transition == TransitionCamera::TvSs);
    const CameraCue gold_play = schedule_disc_camera(gold, 1000, three, keys);
    CHECK(gold_play.road == RoadCamera::S02);
    const CameraCue gold_mix = schedule_disc_camera(gold, three[1].start_ms - 1, three, keys);
    CHECK(gold_mix.kind == CameraCueKind::Transition);
    CHECK(gold_mix.transition == TransitionCamera::TvBs);
    CHECK(gold_mix.section == 1);
    const CameraCue gold_mid = schedule_disc_camera(gold, three[1].start_ms + 1000, three, keys);
    CHECK(gold_mid.kind == CameraCueKind::Play);
    CHECK(gold_mid.road == RoadCamera::B01);
    const CameraCue gold_back = schedule_disc_camera(gold, three[2].start_ms - 1, three, keys);
    CHECK(gold_back.kind == CameraCueKind::Transition);
    CHECK(gold_back.transition == TransitionCamera::TvBs);
    const CameraCue gold_last = schedule_disc_camera(gold, three[2].start_ms + 1000, three, keys);
    CHECK(gold_last.road == RoadCamera::S02);
    CHECK(gold_last.kind == CameraCueKind::Play);
}

TEST_CASE("the gold shift rises, returns, and leaves the figure to the right") {
    using namespace oscilline;
    const GoldShiftPs early = gold_shift_ps(10000);
    CHECK(early.ribbon_dy == doctest::Approx(0.f));
    CHECK(early.figure_dx == doctest::Approx(0.f));

    const GoldShiftPs rising = gold_shift_ps(kGoldRibbonRiseStartMs + kGoldRibbonRiseMs / 2);
    CHECK(rising.ribbon_dy == doctest::Approx(0.5f * kGoldRibbonRisePs));

    const GoldShiftPs up = gold_shift_ps(kGoldRibbonRiseStartMs + kGoldRibbonRiseMs);
    CHECK(up.ribbon_dy == doctest::Approx(kGoldRibbonRisePs));
    CHECK(up.figure_dx == doctest::Approx(0.f));

    const std::int64_t shifted = kGoldFigureShiftStartMs + kGoldFigureShiftMs;
    const GoldShiftPs both = gold_shift_ps(shifted);
    CHECK(both.ribbon_dy == doctest::Approx(kGoldRibbonRisePs));
    CHECK(both.figure_dx == doctest::Approx(kGoldFigureShiftPs));

    const std::int64_t down =
        kGoldRibbonRiseStartMs + kGoldRibbonRiseMs + kGoldRibbonHoldMs + kGoldRibbonFallMs;
    const GoldShiftPs after = gold_shift_ps(down + 1000);
    CHECK(after.ribbon_dy == doctest::Approx(0.f));
    CHECK(after.figure_dx == doctest::Approx(kGoldFigureShiftPs));
}

TEST_CASE("stage-start pans last eight seconds and later pans stop before hit windows") {
    using namespace oscilline;
    const auto sections = course2_sections();
    const CameraKeyCounts keys = observed_keys();
    const std::int64_t intro = sections[0].start_ms;
    const std::int64_t full = transition_duration_ms(kObservedTransitionKeys);
    const std::int64_t stage = intro - full;
    const std::int64_t next = sections[1].start_ms;

    const std::vector<CameraObstacle> distant{{intro + 30000, 1000}};
    const CameraCue opening_start = schedule_disc_camera(0, stage, sections, keys, distant, false);
    REQUIRE(opening_start.kind == CameraCueKind::Transition);
    CHECK(opening_start.key_index == doctest::Approx(0.f));
    const CameraCue opening_wait = schedule_disc_camera(
        0, stage + kLoopingPanStartDelayMs - 1, sections, keys, distant, false);
    CHECK(opening_wait.key_index == doctest::Approx(0.f));
    const CameraCue opening_motion = schedule_disc_camera(
        0, stage + kLoopingPanStartDelayMs + 1200, sections, keys, distant, false);
    CHECK(opening_motion.key_index ==
          doctest::Approx(kObservedTransitionKeys * (1200.f / 7750.f)).epsilon(0.01f));
    const CameraCue opening_done =
        schedule_disc_camera(0, stage + kLoopingPanLongMs, sections, keys, distant, false);
    CHECK_FALSE(opening_done.playing);

    // An obstacle sits inside the old intro. Both courses still spin, and the
    // pan has finished a second before that hit.
    const std::vector<CameraObstacle> intro_crowd{{intro - 1000, 400}};
    const std::int64_t intro_deadline =
        (intro - 1000) - kCameraInputLeadMs - kCameraPanWindowLeadMs;
    const CameraCue course1_intro =
        schedule_disc_camera(0, stage, sections, keys, intro_crowd, false);
    CHECK(course1_intro.kind == CameraCueKind::Transition);
    CHECK(course1_intro.playing);
    CHECK(course1_intro.section == 0);
    CHECK(course1_intro.key_index > 0.f);
    const CameraCue course2_intro = schedule_disc_camera(
        1, stage + (intro_deadline - stage) / 2, sections, keys, intro_crowd, true);
    CHECK(course2_intro.kind == CameraCueKind::Transition);
    CHECK(course2_intro.playing);
    const CameraCue intro_last =
        schedule_disc_camera(1, intro_deadline - 1, sections, keys, intro_crowd, true);
    CHECK(intro_last.playing);
    CHECK(intro_last.key_index > static_cast<float>(kObservedTransitionKeys) - 2.f);
    const CameraCue intro_clear =
        schedule_disc_camera(1, intro_deadline, sections, keys, intro_crowd, true);
    CHECK_FALSE(intro_clear.playing);
    CHECK(intro_clear.kind == CameraCueKind::Play);

    // The opening remains an eight-second pan even with an early hit window.
    const std::vector<CameraObstacle> soon{{stage + 1500, 0}};
    const std::int64_t soon_deadline = (stage + 1500) - kCameraInputLeadMs - kCameraPanWindowLeadMs;
    REQUIRE(soon_deadline - stage < kShortTransitionMinMs);
    REQUIRE(soon_deadline > stage);
    const CameraCue brief = schedule_disc_camera(1, stage, sections, keys, soon, true);
    CHECK(brief.kind == CameraCueKind::Transition);
    CHECK(brief.playing);
    CHECK(brief.key_index > 0.f);
    const CameraCue brief_done = schedule_disc_camera(1, soon_deadline, sections, keys, soon, true);
    CHECK_FALSE(brief_done.playing);

    // Fourteen seconds of quiet. The later pan uses its eight-second length and
    // stops before the next hit window.
    const std::vector<CameraObstacle> wide{
        {next - 14000, 200},
        {next + 1000, 200},
    };
    const std::int64_t pan_begin = next - 8100;
    const CameraCue gap_open = schedule_disc_camera(1, pan_begin, sections, keys, wide, true);
    CHECK(gap_open.kind == CameraCueKind::Transition);
    CHECK(gap_open.playing);
    CHECK(gap_open.key_index == doctest::Approx(0.f));
    const CameraCue gap_mid = schedule_disc_camera(1, pan_begin + 4000, sections, keys, wide, true);
    CHECK(gap_mid.playing);
    CHECK(gap_mid.key_index == doctest::Approx(kObservedTransitionKeys / 2.f));
    const CameraCue gap_end = schedule_disc_camera(1, next - 1, sections, keys, wide, true);
    CHECK_FALSE(gap_end.playing);
    const CameraCue lead = schedule_disc_camera(1, next, sections, keys, wide, true);
    CHECK(lead.kind == CameraCueKind::Play);
    CHECK(lead.road == RoadCamera::S02);
    CHECK_FALSE(lead.playing);
    const CameraCue during_lead = schedule_disc_camera(1, next + 100, sections, keys, wide, true);
    CHECK(during_lead.kind == CameraCueKind::Play);
    CHECK_FALSE(during_lead.playing);

    // Under 4 s between obstacles: no spin, a short blend into the next camera.
    const std::vector<CameraObstacle> crowded{
        {next - 2000, 200},
        {next + 1000, 200},
    };
    const CameraCue easing = schedule_disc_camera(1, next - 200, sections, keys, crowded, true);
    CHECK(easing.kind == CameraCueKind::Blend);
    CHECK(easing.blend > 0.f);
    CHECK(easing.blend < 1.f);
    CHECK(easing.road == RoadCamera::S02);
    CHECK(easing.key_index == doctest::Approx(0.f));
    CHECK(easing.blend_key == doctest::Approx(static_cast<float>(kObservedS01Keys - 1)));
    const CameraCue course1_blend =
        schedule_disc_camera(0, next - 200, sections, keys, crowded, false);
    CHECK(course1_blend.kind == CameraCueKind::Blend);
    const CameraCue arrived = schedule_disc_camera(1, next, sections, keys, crowded, true);
    CHECK(arrived.kind == CameraCueKind::Play);
    CHECK(arrived.road == RoadCamera::S02);
    CHECK(arrived.key_index == doctest::Approx(0.f));

    // After the last obstacle, a primary-length pan can run in the tail gap.
    const std::vector<CameraObstacle> early{{sections[0].end_ms - 1000, 500}};
    const std::int64_t quiet = sections[0].end_ms - 1000;
    const CameraCue tail_open = schedule_disc_camera(1, quiet, sections, keys, early, true);
    CHECK(tail_open.kind == CameraCueKind::Transition);
    CHECK(tail_open.playing);
    CHECK(tail_open.key_index == doctest::Approx(0.f));
    const CameraCue tail_later = schedule_disc_camera(1, quiet + 2000, sections, keys, early, true);
    CHECK(tail_later.playing);
    CHECK(tail_later.key_index > 1.f);
}

TEST_CASE("gaps under four seconds skip looping pans") {
    using namespace oscilline;
    const CameraKeyCounts keys = observed_keys();

    const std::vector<CameraObstacle> short_gap{
        {1000, 0, 0, false},
        {5100, 0, 4999, true},
    };
    const CameraCue short_gap_cue = schedule_held_gap_camera(3000, keys, short_gap, 20000);
    CHECK(short_gap_cue.kind == CameraCueKind::Play);
    CHECK_FALSE(short_gap_cue.playing);

    const std::vector<CameraObstacle> minimum_gap{
        {1000, 0, 0, false},
        {5201, 0, 5100, true},
    };
    const CameraCue minimum_gap_cue = schedule_held_gap_camera(1000, keys, minimum_gap, 20000);
    CHECK(minimum_gap_cue.kind == CameraCueKind::Transition);
    CHECK(minimum_gap_cue.playing);
    CHECK(minimum_gap_cue.key_index == doctest::Approx(0.f));

    const std::vector<CameraObstacle> short_tail{{1000, 0, 0, false}};
    const CameraCue short_tail_cue = schedule_held_gap_camera(3000, keys, short_tail, 4999);
    CHECK(short_tail_cue.kind == CameraCueKind::Play);
    CHECK_FALSE(short_tail_cue.playing);
}

TEST_CASE("looping pans use eight or four seconds and end before hit windows") {
    using namespace oscilline;
    const CameraKeyCounts keys = observed_keys();

    const std::vector<CameraObstacle> long_gap{
        {1000, 0, 0, false},
        {20000, 12000, 19500, true},
    };
    const CameraCue after_spawn = schedule_held_gap_camera(15000, keys, long_gap, 30000);
    CHECK(after_spawn.kind == CameraCueKind::Transition);
    CHECK(after_spawn.playing);
    CHECK(after_spawn.key_index == doctest::Approx(kObservedTransitionKeys * (3600.f / 8000.f)));
    const CameraCue eight_seconds_done = schedule_held_gap_camera(19400, keys, long_gap, 30000);
    CHECK_FALSE(eight_seconds_done.playing);

    const std::vector<CameraObstacle> medium_gap{
        {1000, 0, 0, false},
        {8000, 0, 7900, true},
    };
    const CameraCue four_second_mid = schedule_held_gap_camera(5800, keys, medium_gap, 12000);
    CHECK(four_second_mid.kind == CameraCueKind::Transition);
    CHECK(four_second_mid.playing);
    CHECK(four_second_mid.key_index == doctest::Approx(kObservedTransitionKeys / 2.f));
    const CameraCue four_seconds_done = schedule_held_gap_camera(7800, keys, medium_gap, 12000);
    CHECK_FALSE(four_seconds_done.playing);
}

TEST_CASE("a chart without sections fits the looping pan to each quiet gap") {
    using namespace oscilline;
    const CameraKeyCounts keys = observed_keys();
    const std::vector<CameraObstacle> tight{{10000, 1000}, {12000, 1000}};
    const CameraCue crowded = schedule_held_gap_camera(11000, keys, tight, 30000);
    CHECK(crowded.kind == CameraCueKind::Play);
    CHECK(crowded.road == RoadCamera::S01);
    CHECK(crowded.key_index == doctest::Approx(0.f));

    const std::vector<CameraObstacle> open{{10000, 1000}, {20000, 1000}};
    const std::int64_t stage = -static_cast<std::int64_t>(kCourseStartDelayMs);
    const CameraCue opening = schedule_held_gap_camera(stage, keys, open, 40000);
    CHECK(opening.kind == CameraCueKind::Transition);
    CHECK(opening.playing);
    CHECK(opening.key_index == doctest::Approx(0.f));

    const CameraCue before_gap_pan = schedule_held_gap_camera(10000, keys, open, 40000);
    CHECK(before_gap_pan.kind == CameraCueKind::Play);
    const CameraCue spinning = schedule_held_gap_camera(10900, keys, open, 40000);
    CHECK(spinning.kind == CameraCueKind::Transition);
    CHECK(spinning.playing);
    CHECK(spinning.transition == TransitionCamera::TvSs);
    CHECK(spinning.key_index == doctest::Approx(0.f));
    const CameraCue mid_gap = schedule_held_gap_camera(14900, keys, open, 40000);
    CHECK(mid_gap.playing);
    CHECK(mid_gap.key_index == doctest::Approx(kObservedTransitionKeys / 2.f));
    const CameraCue after_gap_delay = schedule_held_gap_camera(16900, keys, open, 40000);
    CHECK(after_gap_delay.playing);
    CHECK(after_gap_delay.key_index == doctest::Approx(kObservedTransitionKeys * 3.f / 4.f));
    const CameraCue between = schedule_held_gap_camera(19500, keys, open, 40000);
    CHECK(between.kind == CameraCueKind::Play);

    // Three seconds of prelude before the lead still runs the stage-start pan.
    const std::vector<CameraObstacle> brief{{-4000, 0}};
    const CameraCue short_open = schedule_held_gap_camera(stage, keys, brief, 20000);
    CHECK(short_open.playing);
    CHECK(short_open.key_index > 0.f);
    const CameraCue short_done = schedule_held_gap_camera(-5000, keys, brief, 20000);
    CHECK_FALSE(short_done.playing);

    const std::vector<CameraObstacle> tail{{1000, 500}};
    const CameraCue late = schedule_held_gap_camera(5000, keys, tail, 20000);
    CHECK(late.kind == CameraCueKind::Transition);
    CHECK(late.playing);
    CHECK(late.key_index > 1.f);
    const CameraCue before = schedule_held_gap_camera(200, keys, tail, 20000);
    CHECK(before.kind == CameraCueKind::Play);
}
