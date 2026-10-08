// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Stretches each section's road file and plays the transition into the next.

#include "oscilline/course/camera_schedule.hpp"

#include "oscilline/course/camera.hpp"
#include "oscilline/course/mapping.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace oscilline {
namespace {

// Courses 1–4 and course 5 section 1 were read from the PAL camera timer.
// Course 5 sections 2 and 3 are guesses that make the observed TV_BS an S/B
// mix. Course 6 was not observed; S02, S01, S02 is a placeholder.
struct CourseCameraRow {
    RoadCamera section[kCameraSectionsPerCourse];
    bool guess[kCameraSectionsPerCourse];
};

constexpr CourseCameraRow kCourseCameras[kCameraScheduleCourses] = {
    {{RoadCamera::S01, RoadCamera::S01, RoadCamera::S01}, {false, false, false}},
    {{RoadCamera::S01, RoadCamera::S02, RoadCamera::S01}, {false, false, false}},
    {{RoadCamera::S02, RoadCamera::S01, RoadCamera::S02}, {false, false, false}},
    {{RoadCamera::B01, RoadCamera::B01, RoadCamera::B01}, {false, false, false}},
    {{RoadCamera::S02, RoadCamera::B01, RoadCamera::S02}, {false, true, true}},
    {{RoadCamera::S02, RoadCamera::S01, RoadCamera::S02}, {true, true, true}},
};

constexpr int kPatternFixed = 1;
constexpr int kPatternDistribution = 2;
constexpr std::int32_t kIntMax = 0x7fffffff;

int section_slot(int section) {
    if (section < 0) {
        return 0;
    }
    if (section >= kCameraSectionsPerCourse) {
        return kCameraSectionsPerCourse - 1;
    }
    return section;
}

bool known_course(int course_index) {
    return course_index >= 0 && course_index < kCameraScheduleCourses;
}

int keys_for_road(CameraKeyCounts keys, RoadCamera road) {
    switch (road) {
    case RoadCamera::S02:
        return keys.s02;
    case RoadCamera::B01:
        return keys.b01;
    case RoadCamera::S01:
        return keys.s01;
    }
    return 0;
}

int keys_for_transition(CameraKeyCounts keys, TransitionCamera transition) {
    switch (transition) {
    case TransitionCamera::TvBb:
        return keys.tv_bb;
    case TransitionCamera::TvBs:
        return keys.tv_bs;
    case TransitionCamera::TvSs:
        return keys.tv_ss;
    }
    return 0;
}

CameraCue fallback_s01(int section) {
    CameraCue cue;
    cue.kind = CameraCueKind::FallbackS01;
    cue.road = RoadCamera::S01;
    cue.section = section;
    cue.key_index = 0.f;
    cue.file_missing = true;
    return cue;
}

bool road_missing(CameraCue& cue, CameraKeyCounts keys, RoadCamera road, int section) {
    if (keys_for_road(keys, road) > 0) {
        return false;
    }
    cue = fallback_s01(section);
    return true;
}

// Same rounding as the course mapper's units_to_ms / ms_to_units.
std::int32_t units_to_ms(std::int64_t units, double units_per_second) {
    if (units <= 0 || !(units_per_second > 0.0)) {
        return 0;
    }
    const double ms = static_cast<double>(units) * 1000.0 / units_per_second;
    if (ms >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    return static_cast<std::int32_t>(std::llround(ms));
}

std::int32_t ms_to_units(std::int32_t ms, double units_per_second) {
    if (ms <= 0 || !(units_per_second > 0.0)) {
        return 0;
    }
    const double units = static_cast<double>(ms) * units_per_second / 1000.0;
    if (units >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    return static_cast<std::int32_t>(std::llround(units));
}

float smooth01(float u) {
    if (!(u > 0.f)) {
        return 0.f;
    }
    if (u >= 1.f) {
        return 1.f;
    }
    return u * u * (3.f - 2.f * u);
}

std::int64_t obstacle_spawn_ms(const CameraObstacle& obstacle) {
    const std::int64_t approach = obstacle.approach_ms > 0 ? obstacle.approach_ms : 0;
    return obstacle.hit_ms - approach;
}

// Clear time immediately before `boundary`. No obstacle yields an unlimited gap.
std::optional<std::int64_t> clear_ms_before(std::int64_t boundary,
                                            std::span<const CameraObstacle> obstacles) {
    bool saw = false;
    std::int64_t blocked = 0;
    for (const CameraObstacle& obstacle : obstacles) {
        if (obstacle_spawn_ms(obstacle) >= boundary) {
            continue;
        }
        if (!saw || obstacle.hit_ms > blocked) {
            blocked = obstacle.hit_ms;
            saw = true;
        }
    }
    if (!saw) {
        return std::nullopt;
    }
    if (blocked >= boundary) {
        return 0;
    }
    return boundary - blocked;
}

struct BoundaryPlan {
    bool spin = false;
    bool shortened = false;
    std::int64_t spin_begin = 0;
    bool blend = false;
    std::int64_t blend_begin = 0;
    std::int64_t blend_end = 0;
};

// Full spin when the gap can hold it. Otherwise compress into the gap, or skip
// and blend so the next play key does not pop in.
BoundaryPlan plan_boundary(int index,
                           std::int64_t start_ms,
                           int transition_keys,
                           std::span<const CameraObstacle> obstacles,
                           bool gate) {
    BoundaryPlan plan;
    const std::int64_t full = transition_duration_ms(transition_keys);
    // Later sections always need a quiet gap. The intro does too when gating
    // is on (courses 2+ and custom charts). Course 1's intro stays on the
    // watched spin.
    const bool require_gap = gate || index > 0;
    if (!require_gap) {
        if (transition_keys > 0 && full > 0) {
            plan.spin = true;
            plan.spin_begin = start_ms - full;
        }
        return plan;
    }
    const std::optional<std::int64_t> gap = clear_ms_before(start_ms, obstacles);
    const bool unlimited = !gap.has_value();
    const std::int64_t clear = unlimited ? full : *gap;
    if (transition_keys > 0 && full > 0 && clear >= full) {
        plan.spin = true;
        plan.spin_begin = start_ms - full;
        return plan;
    }
    if (transition_keys > 0 && clear >= kShortTransitionMinMs) {
        plan.spin = true;
        plan.shortened = true;
        plan.spin_begin = start_ms - clear;
        return plan;
    }
    plan.blend = true;
    std::int64_t handoff = kCameraHandoffMs;
    if (!unlimited && clear > 0 && clear < handoff) {
        handoff = clear;
    }
    if (handoff < 1) {
        handoff = 1;
    }
    if (unlimited || clear > 0) {
        plan.blend_begin = start_ms - handoff;
        plan.blend_end = start_ms;
    } else {
        plan.blend_begin = start_ms;
        plan.blend_end = start_ms + kCameraHandoffMs;
    }
    return plan;
}

float spin_key_index(std::int64_t audio_ms,
                     std::int64_t begin_ms,
                     std::int64_t end_ms,
                     int key_count,
                     bool shortened) {
    if (!shortened) {
        return disc_camera_key_index(audio_ms - begin_ms);
    }
    const std::int64_t span = end_ms - begin_ms;
    if (span <= 0 || key_count <= 0) {
        return 0.f;
    }
    return static_cast<float>(audio_ms - begin_ms) * static_cast<float>(key_count) /
           static_cast<float>(span);
}

struct PanWindow {
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::int64_t duration_ms = 0;
    bool opening = false;
};

float looping_pan_key_index(std::int64_t audio_ms, const PanWindow& window, int key_count) {
    const std::int64_t delay = window.opening ? kLoopingPanStartDelayMs : 0;
    const std::int64_t motion_begin = window.begin + delay;
    if (audio_ms <= motion_begin || key_count <= 0) {
        return 0.f;
    }
    const std::int64_t motion_duration = window.duration_ms - delay;
    if (motion_duration <= 0 || audio_ms >= window.end) {
        return static_cast<float>(key_count);
    }
    const std::int64_t elapsed = audio_ms - motion_begin;
    return static_cast<float>(std::min(elapsed, motion_duration)) * static_cast<float>(key_count) /
           static_cast<float>(motion_duration);
}

// Pans may continue after obstacles enter the screen, but must end before the
// first hit window opens.
std::int64_t pan_deadline(const CameraObstacle& obstacle) {
    const std::int64_t window_open =
        obstacle.has_window ? obstacle.window_open_ms : obstacle.hit_ms - kCameraInputLeadMs;
    return window_open - kCameraPanWindowLeadMs;
}

// Stage-start pans use eight seconds. Other gaps need at least four seconds;
// longer gaps prefer eight seconds, then four. `tail` fills a quiet ending.
std::int64_t preferred_pan_duration(std::int64_t available_ms) {
    if (available_ms < kLoopingPanShortMs) {
        return 0;
    }
    if (available_ms >= kLoopingPanLongMs) {
        return kLoopingPanLongMs;
    }
    return kLoopingPanShortMs;
}

std::vector<PanWindow> collect_pan_windows(std::int64_t stage_begin,
                                           std::int64_t horizon_ms,
                                           std::span<const CameraObstacle> obstacles,
                                           bool tail) {
    std::vector<PanWindow> windows;
    if (obstacles.empty()) {
        return windows;
    }
    std::vector<CameraObstacle> ordered(obstacles.begin(), obstacles.end());
    std::stable_sort(
        ordered.begin(), ordered.end(), [](const CameraObstacle& a, const CameraObstacle& b) {
            return a.hit_ms < b.hit_ms;
        });

    const std::int64_t opening_deadline = pan_deadline(ordered.front());
    const std::int64_t opening_duration = kLoopingPanLongMs;
    std::int64_t opening_begin = stage_begin;
    std::int64_t opening_end = stage_begin + opening_duration;
    if (opening_deadline < opening_end) {
        opening_end = opening_deadline;
        opening_begin = opening_end - opening_duration;
    }
    if (opening_end > opening_begin) {
        windows.push_back(PanWindow{opening_begin, opening_end, opening_duration, true});
    }

    for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
        const std::int64_t available_begin = ordered[i].hit_ms;
        const std::int64_t end = pan_deadline(ordered[i + 1]);
        const std::int64_t duration = preferred_pan_duration(end - available_begin);
        if (duration > 0 && end > available_begin) {
            // Anchor the pan to the next hit window so it finishes just before
            // the player needs the play camera, even when the gap is longer.
            windows.push_back(PanWindow{end - duration, end, duration, false});
        }
    }

    if (tail && horizon_ms > ordered.back().hit_ms) {
        const std::int64_t begin = ordered.back().hit_ms;
        const std::int64_t duration = preferred_pan_duration(horizon_ms - begin);
        if (duration > 0) {
            windows.push_back(PanWindow{begin, begin + duration, duration, false});
        }
    }
    return windows;
}

struct PanChoice {
    TransitionCamera transition = TransitionCamera::TvSs;
    int section = 0;
    int key_count = 0;
};

// The section boundary the pan crosses, or a same-family loop when it stays
// inside one section. The opening that ends before section 1 still uses that
// intro transition.
PanChoice pan_choice(int course_index,
                     std::span<const CameraSectionSpan> sections,
                     CameraKeyCounts keys,
                     const PanWindow& window) {
    int into = -1;
    for (int index = 0; index < static_cast<int>(sections.size()); ++index) {
        const std::int64_t start = sections[static_cast<std::size_t>(index)].start_ms;
        if (start > window.begin && start <= window.end) {
            into = index;
            break;
        }
    }
    if (window.opening && into < 0) {
        into = 0;
    }
    PanChoice choice;
    if (into >= 0) {
        choice.section = into;
        choice.transition = transition_into_section(course_index, into);
    } else {
        int current = 0;
        for (int index = static_cast<int>(sections.size()) - 1; index >= 0; --index) {
            if (window.begin >= sections[static_cast<std::size_t>(index)].start_ms) {
                current = index;
                break;
            }
        }
        choice.section = current;
        const RoadCamera road = course_section_camera(course_index, current);
        choice.transition =
            road == RoadCamera::B01 ? TransitionCamera::TvBb : TransitionCamera::TvSs;
    }
    choice.key_count = keys_for_transition(keys, choice.transition);
    return choice;
}

float ramp(std::int64_t time_ms, std::int64_t start_ms, std::int64_t duration_ms) {
    if (duration_ms <= 0) {
        return time_ms >= start_ms ? 1.f : 0.f;
    }
    return smooth01(static_cast<float>(time_ms - start_ms) / static_cast<float>(duration_ms));
}

} // namespace

RoadCamera course_section_camera(int course_index, int section) {
    if (!known_course(course_index)) {
        return RoadCamera::S01;
    }
    return kCourseCameras[course_index].section[section_slot(section)];
}

bool course_section_camera_is_guess(int course_index, int section) {
    if (!known_course(course_index)) {
        return true;
    }
    if (section < 0 || section >= kCameraSectionsPerCourse) {
        return true;
    }
    return kCourseCameras[course_index].guess[section];
}

TransitionCamera transition_into_section(int course_index, int section) {
    const RoadCamera next = course_section_camera(course_index, section);
    if (section <= 0) {
        return next == RoadCamera::B01 ? TransitionCamera::TvBb : TransitionCamera::TvSs;
    }
    const RoadCamera previous = course_section_camera(course_index, section - 1);
    const bool previous_b = previous == RoadCamera::B01;
    const bool next_b = next == RoadCamera::B01;
    if (previous_b != next_b) {
        return TransitionCamera::TvBs;
    }
    return next_b ? TransitionCamera::TvBb : TransitionCamera::TvSs;
}

float stretched_key_index(std::int64_t time_ms,
                          std::int64_t start_ms,
                          std::int64_t end_ms,
                          int key_count) {
    if (key_count <= 1) {
        return 0.f;
    }
    const float last = static_cast<float>(key_count - 1);
    if (end_ms <= start_ms || time_ms <= start_ms) {
        return 0.f;
    }
    if (time_ms >= end_ms) {
        return last;
    }
    const std::int64_t span = end_ms - start_ms;
    // Observed ANC timer: key index in 16.16, so the last key is (count - 1) << 16.
    const std::int64_t timer_last = static_cast<std::int64_t>(key_count - 1) * kAncTimerScale;
    const std::int64_t timer = (time_ms - start_ms) * timer_last / span;
    return static_cast<float>(timer) / static_cast<float>(kAncTimerScale);
}

std::int64_t transition_duration_ms(int key_count) {
    if (key_count <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(key_count) * 1000 /
           static_cast<std::int64_t>(kDiscCameraKeysPerSecond);
}

CameraCue schedule_disc_camera(int course_index,
                               std::int64_t audio_ms,
                               std::span<const CameraSectionSpan> sections,
                               CameraKeyCounts keys,
                               std::span<const CameraObstacle> obstacles,
                               bool gate_transitions) {
    if (sections.empty()) {
        return fallback_s01(0);
    }
    const int count = static_cast<int>(sections.size());
    std::vector<PanWindow> windows;
    if (!obstacles.empty()) {
        // Obstacle charts use fixed 8 s or 4 s looping pans, ending before the
        // next hit window. Empty charts keep the watched boundary spins below.
        const TransitionCamera intro = transition_into_section(course_index, 0);
        const std::int64_t full = transition_duration_ms(keys_for_transition(keys, intro));
        std::int64_t stage_begin = sections[0].start_ms;
        if (full > 0) {
            stage_begin -= full;
        }
        const std::int64_t horizon = sections[static_cast<std::size_t>(count - 1)].end_ms;
        windows = collect_pan_windows(stage_begin, horizon, obstacles, true);
        for (const PanWindow& window : windows) {
            if (audio_ms < window.begin || audio_ms >= window.end) {
                continue;
            }
            const PanChoice choice = pan_choice(course_index, sections, keys, window);
            if (choice.key_count <= 0) {
                continue;
            }
            CameraCue cue;
            cue.kind = CameraCueKind::Transition;
            cue.transition = choice.transition;
            cue.section = choice.section;
            cue.playing = true;
            cue.key_index = looping_pan_key_index(audio_ms, window, choice.key_count);
            return cue;
        }
    } else {
        for (int index = 0; index < count; ++index) {
            const TransitionCamera transition = transition_into_section(course_index, index);
            const int transition_keys = keys_for_transition(keys, transition);
            const std::int64_t start = sections[static_cast<std::size_t>(index)].start_ms;
            const BoundaryPlan plan =
                plan_boundary(index, start, transition_keys, obstacles, gate_transitions);
            if (plan.spin && audio_ms >= plan.spin_begin && audio_ms < start) {
                CameraCue cue;
                cue.kind = CameraCueKind::Transition;
                cue.transition = transition;
                cue.section = index;
                cue.playing = true;
                cue.key_index = spin_key_index(
                    audio_ms, plan.spin_begin, start, transition_keys, plan.shortened);
                return cue;
            }
        }
    }

    for (int index = 1; index < count; ++index) {
        const TransitionCamera transition = transition_into_section(course_index, index);
        const int transition_keys = keys_for_transition(keys, transition);
        const CameraSectionSpan& section = sections[static_cast<std::size_t>(index)];
        // Later sections blend even when the intro is ungated (course 1).
        const BoundaryPlan plan =
            plan_boundary(index, section.start_ms, transition_keys, obstacles, true);
        if (!plan.blend || plan.blend_end <= plan.blend_begin || audio_ms < plan.blend_begin ||
            audio_ms >= plan.blend_end) {
            continue;
        }
        const RoadCamera next = course_section_camera(course_index, index);
        const RoadCamera previous = course_section_camera(course_index, index - 1);
        const int next_keys = keys_for_road(keys, next);
        const int previous_keys = keys_for_road(keys, previous);
        CameraCue cue;
        cue.kind = CameraCueKind::Blend;
        cue.section = index;
        cue.road = next;
        cue.key_index = stretched_key_index(
            audio_ms, section.start_ms, section.end_ms, next_keys > 0 ? next_keys : 1);
        cue.blend_road = previous;
        cue.blend_key = previous_keys > 1 ? static_cast<float>(previous_keys - 1) : 0.f;
        const float u = static_cast<float>(audio_ms - plan.blend_begin) /
                        static_cast<float>(plan.blend_end - plan.blend_begin);
        cue.blend = smooth01(u);
        return cue;
    }

    for (int index = 0; index < count; ++index) {
        const CameraSectionSpan& section = sections[static_cast<std::size_t>(index)];
        if (section.end_ms < section.start_ms || audio_ms < section.start_ms ||
            audio_ms > section.end_ms) {
            continue;
        }
        const RoadCamera road = course_section_camera(course_index, index);
        CameraCue cue;
        cue.section = index;
        cue.road = road;
        if (road_missing(cue, keys, road, index)) {
            return cue;
        }
        cue.kind = CameraCueKind::Play;
        cue.key_index = stretched_key_index(
            audio_ms, section.start_ms, section.end_ms, keys_for_road(keys, road));
        return cue;
    }

    for (int index = count - 1; index >= 0; --index) {
        const CameraSectionSpan& section = sections[static_cast<std::size_t>(index)];
        if (audio_ms <= section.end_ms) {
            continue;
        }
        if (index + 1 < count &&
            audio_ms >= sections[static_cast<std::size_t>(index + 1)].start_ms) {
            continue;
        }
        const RoadCamera road = course_section_camera(course_index, index);
        CameraCue cue;
        cue.section = index;
        cue.road = road;
        if (road_missing(cue, keys, road, index)) {
            return cue;
        }
        const int road_keys = keys_for_road(keys, road);
        cue.kind = CameraCueKind::Hold;
        cue.key_index = road_keys > 1 ? static_cast<float>(road_keys - 1) : 0.f;
        return cue;
    }

    if (!windows.empty() && audio_ms < sections[0].start_ms) {
        for (const PanWindow& window : windows) {
            if (!window.opening || audio_ms < window.end) {
                continue;
            }
            // The pan finished early so the lead is a normal play camera.
            const RoadCamera road = course_section_camera(course_index, 0);
            CameraCue cue;
            cue.section = 0;
            cue.road = road;
            if (road_missing(cue, keys, road, 0)) {
                return cue;
            }
            cue.kind = CameraCueKind::Play;
            cue.key_index = 0.f;
            return cue;
        }
    }

    const TransitionCamera intro = transition_into_section(course_index, 0);
    if (keys_for_transition(keys, intro) <= 0) {
        return fallback_s01(0);
    }
    CameraCue cue;
    cue.kind = CameraCueKind::Transition;
    cue.transition = intro;
    cue.section = 0;
    cue.playing = false;
    cue.key_index = 0.f;
    return cue;
}

std::vector<CameraSectionSpan>
camera_sections_from_patterns(std::span<const FslPatternSegment> segments,
                              std::int32_t audio_duration_ms,
                              double units_per_second) {
    std::vector<CameraSectionSpan> spans;
    if (!(units_per_second > 0.0) || !std::isfinite(units_per_second) || segments.empty()) {
        return spans;
    }
    std::vector<std::size_t> order(segments.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return segments[a].start_time < segments[b].start_time;
    });

    for (std::size_t n = 0; n < order.size(); ++n) {
        const FslPatternSegment& segment = segments[order[n]];
        if (segment.pattern_type != kPatternFixed && segment.pattern_type != kPatternDistribution) {
            continue;
        }
        std::int32_t end_units = 0;
        if (n + 1 < order.size()) {
            end_units = segments[order[n + 1]].start_time;
        } else if (audio_duration_ms > 0) {
            end_units = ms_to_units(audio_duration_ms, units_per_second);
        } else {
            const std::int32_t tail = ms_to_units(kTailMsWithoutAudio, units_per_second);
            end_units = segment.start_time > kIntMax - tail ? kIntMax : segment.start_time + tail;
        }
        if (end_units <= segment.start_time) {
            continue;
        }
        CameraSectionSpan span;
        span.start_ms = units_to_ms(segment.start_time, units_per_second);
        span.end_ms = units_to_ms(end_units, units_per_second);
        if (span.end_ms <= span.start_ms) {
            continue;
        }
        spans.push_back(span);
    }
    return spans;
}

CameraCue schedule_held_gap_camera(std::int64_t audio_ms,
                                   CameraKeyCounts keys,
                                   std::span<const CameraObstacle> obstacles,
                                   std::int64_t audio_end_ms) {
    if (keys.s01 <= 0) {
        return fallback_s01(0);
    }
    CameraCue hold;
    hold.kind = CameraCueKind::Play;
    hold.road = RoadCamera::S01;
    hold.section = 0;
    hold.key_index = 0.f;

    if (keys.tv_ss <= 0 || obstacles.empty()) {
        return hold;
    }

    const std::int64_t stage_begin = -static_cast<std::int64_t>(kCourseStartDelayMs);
    const std::vector<PanWindow> windows =
        collect_pan_windows(stage_begin, audio_end_ms, obstacles, audio_end_ms > 0);
    for (const PanWindow& window : windows) {
        if (audio_ms < window.begin || audio_ms >= window.end) {
            continue;
        }
        CameraCue cue;
        cue.kind = CameraCueKind::Transition;
        cue.transition = TransitionCamera::TvSs;
        cue.section = 0;
        cue.playing = true;
        cue.key_index = looping_pan_key_index(audio_ms, window, keys.tv_ss);
        return cue;
    }
    return hold;
}

std::vector<CameraSectionSpan> custom_music_camera_sections(std::int32_t duration_ms) {
    std::vector<CameraSectionSpan> spans;
    if (duration_ms <= 0) {
        return spans;
    }
    int count = kCustomMusicSectionAim;
    while (count > 1 && duration_ms / count < kCustomMusicSectionMinMs) {
        --count;
    }
    const std::int64_t whole = duration_ms;
    const std::int64_t span = whole / static_cast<std::int64_t>(count);
    std::int64_t cursor = 0;
    for (int index = 0; index < count; ++index) {
        const std::int64_t end = index + 1 == count ? whole : cursor + span;
        CameraSectionSpan section;
        section.start_ms = cursor;
        section.end_ms = end;
        spans.push_back(section);
        cursor = end;
    }
    return spans;
}

GoldShiftPs gold_shift_ps(std::int64_t audio_ms) {
    GoldShiftPs shift;
    const std::int64_t rise_end = kGoldRibbonRiseStartMs + kGoldRibbonRiseMs;
    const std::int64_t hold_end = rise_end + kGoldRibbonHoldMs;
    const std::int64_t fall_end = hold_end + kGoldRibbonFallMs;
    float raised = 0.f;
    if (audio_ms >= fall_end) {
        raised = 0.f;
    } else if (audio_ms >= hold_end) {
        raised = 1.f - ramp(audio_ms, hold_end, kGoldRibbonFallMs);
    } else if (audio_ms >= rise_end) {
        raised = 1.f;
    } else if (audio_ms >= kGoldRibbonRiseStartMs) {
        raised = ramp(audio_ms, kGoldRibbonRiseStartMs, kGoldRibbonRiseMs);
    }
    shift.ribbon_dy = raised * kGoldRibbonRisePs;
    const float across = ramp(audio_ms, kGoldFigureShiftStartMs, kGoldFigureShiftMs);
    shift.figure_dx = across * kGoldFigureShiftPs;
    return shift;
}

} // namespace oscilline
