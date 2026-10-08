// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Turns an FSL script and emphasis into obstacle hit times.

#pragma once

#include "oscilline/course/attack.hpp"
#include "oscilline/course/camera_schedule.hpp"
#include "oscilline/fsl.hpp"
#include "oscilline/result.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace oscilline {

// Knobs for the course script. The reasoning is in docs/course-mapping.md.
// FSL times are in this many units per second of the audio clock.
inline constexpr double kDefaultUnitsPerSecond = 22050.0;
inline constexpr int kTailMsWithoutAudio = 8000;
inline constexpr int kMinGapMs = 150;
inline constexpr int kLeadInMs = 500;
inline constexpr int kCourseEndTailMs = 1000;
inline constexpr int kMinApproachMs = 1000;
inline constexpr int kMaxApproachMs = 3000;
// The speed value is read as the beat period. An obstacle crosses the screen in
// this many beats, then the result is clamped to the approach range.
inline constexpr int kApproachBeats = 4;
// Screen travel time is doubled to move the stage scroll at half speed.
inline constexpr double kStageScrollSpeedScale = 0.5;
// A shadow within this fraction of a whole number of beats counts as that many beats.
inline constexpr double kShadowBeatTolerance = 1.0 / 16.0;
// A slot is accepted once this many beats short of the shadow have passed, so beat
// jitter does not push an obstacle one whole beat later.
inline constexpr double kSlotAcceptSlackBeats = 0.5;
// Beat periods outside this range are clamped before slots are laid out.
inline constexpr int kMinBeatMs = 150;
inline constexpr int kMaxBeatMs = 2000;
inline constexpr std::int32_t kMinApproachSpeed = 0x40;
inline constexpr int kMaxCourseEvents = 10000;
// FSL table entry 7 is a ~31-minute progression table. It is not a course and
// it does not drive custom-music mode.
inline constexpr int kPlayableCourses = 6;
// Attack charting constants for disc courses live in course/attack.hpp.

struct CourseMapOptions {
    double units_per_second = kDefaultUnitsPerSecond;
    // Beat times on the audio clock, ascending, in milliseconds. When empty the
    // slots are a grid of the speed period laid from each pattern segment start.
    std::span<const std::int32_t> beat_ms;
    // Emphasis per evaluation from attack_emphasis(). When present it
    // places the hits and beat_ms is ignored.
    std::span<const float> emphasis;
};

// Attack times in milliseconds (evaluation stamps) picked from `emphasis` with
// the course's speed, shadow and breaks. Fails like build_course.
// `audio_end_ms` 0 means the length is unknown and a pending attack may still
// commit when the emphasis runs out. A positive length does not commit an
// attack whose commit time falls after the audio.
[[nodiscard]] Result<std::vector<double>> course_attack_ms(const FslFile& file,
                                                           int track_index,
                                                           std::span<const float> emphasis,
                                                           CourseMapOptions options = {},
                                                           std::int32_t audio_end_ms = 0);

struct CourseEvent {
    std::uint8_t obstacle = 0;
    std::int32_t hit_ms = 0;
    std::int32_t approach_ms = 0;
    // Time to cross the screen at this obstacle's control-segment speed.
    // Constant inside a segment, except where a later obstacle is sped up so
    // it does not pass an earlier one.
    std::int32_t scroll_approach_ms = 0;
    // Base-speed beat period for this obstacle, in milliseconds. 0 means the
    // reference tempo. Scroll uses this period; overtake does not change it.
    std::int32_t beat_ms = 0;
};

// All stage obstacles scroll at half their mapped speed. Keep timing consumers
// on this effective travel so visuals, judgment windows, and camera cues agree.
[[nodiscard]] inline std::int32_t stage_scroll_approach_ms(const CourseEvent& event) {
    const std::int64_t travel =
        event.scroll_approach_ms > 0 ? event.scroll_approach_ms : event.approach_ms;
    if (travel <= 0) {
        return 0;
    }
    return static_cast<std::int32_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(travel / kStageScrollSpeedScale),
                               std::numeric_limits<std::int32_t>::max()));
}

struct CourseTimeline {
    int track_index = 0;
    // CD track that plays under this course. Track 1 is the data track.
    int cdda_track = 0;
    std::int32_t duration_ms = 0;
    // Track length on the audio clock. 0 when that length is unknown. The
    // course tail may run past it; spawning and judgment do not.
    std::int32_t audio_end_ms = 0;
    std::vector<CourseEvent> events;
    // Playable spans, in order. Empty when this run has no section schedule.
    // The disc camera stretches one road file across each span. Custom music
    // fills these from equal track slices while a disc camera is in use. The
    // looping pan plays at stage start and fills quiet gaps.
    std::vector<CameraSectionSpan> camera_sections;
    // Courses 5 and 6, and gold custom music on the disc camera.
    bool gold_shift = false;
};

// Smallest i with roll < obstacle_prob[i], or -1 when none match.
// Probabilities are the cumulative buckets from the distribution record.
[[nodiscard]] int distribution_index(std::span<const std::int32_t> obstacle_prob,
                                     std::int32_t roll);

// Beat period (the speed value as time) at `time_ms`, clamped to kMinBeatMs..kMaxBeatMs.
// Fails like build_course when the track or its control data is missing.
[[nodiscard]] Result<double> course_beat_period_ms(const FslFile& file,
                                                   int track_index,
                                                   std::int32_t time_ms,
                                                   CourseMapOptions options = {});

// `audio_duration_ms` is the CD track length, or 0 when that length is unknown.
// Spawn order always follows hit order: no obstacle overtakes an earlier one.
// Random types are chosen after the cuts (audio end; lead-in for beat and grid
// slots only). See course-mapping.md.
[[nodiscard]] Result<CourseTimeline> build_course(const FslFile& file,
                                                  int track_index,
                                                  std::int32_t audio_duration_ms,
                                                  CourseMapOptions options = {});

// True when the obstacle is first seen, or meets the player, after the music
// stops. A zero audio length keeps the obstacle (the length is unknown).
[[nodiscard]] bool
obstacle_after_audio(std::int32_t hit_ms, std::int32_t travel_ms, std::int32_t audio_end_ms);

// Drops those obstacles. They take no miss.
void drop_obstacles_after_audio(std::vector<CourseEvent>& events, std::int32_t audio_end_ms);

} // namespace oscilline
