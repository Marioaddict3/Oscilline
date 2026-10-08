// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Per-section road cameras, transitions, and the Gold shift.

#pragma once

#include "oscilline/fsl.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// ANC playback timer observed in RAM: key index in 16.16 fixed point.
inline constexpr int kAncTimerScale = 65536;

// Key counts observed on the PAL in-play and transition files. Playback uses
// the loaded file's count. These are the sizes the schedule was checked against.
inline constexpr int kObservedS01Keys = 26;
inline constexpr int kObservedS02Keys = 780;
inline constexpr int kObservedB01Keys = 780;
inline constexpr int kObservedTransitionKeys = 240;

// Three in-play sections per disc course.
inline constexpr int kCameraSectionsPerCourse = 3;
inline constexpr int kCameraScheduleCourses = 6;

// Custom music has no FSL spans. Aim for this many equal pieces, but keep
// each one at least about this long. A shorter track uses fewer pieces.
inline constexpr int kCustomMusicSectionAim = kCameraSectionsPerCourse;
inline constexpr std::int32_t kCustomMusicSectionMinMs = 20000;

// In-play road file for one section.
enum class RoadCamera : std::uint8_t { S01, S02, B01 };

// Transition into a section. TV_SS for an S camera, TV_BB for a B camera,
// TV_BS when the previous and next sections are different families.
enum class TransitionCamera : std::uint8_t { TvSs, TvBb, TvBs };

// One playable FSL pattern segment, in milliseconds of course audio.
struct CameraSectionSpan {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
};

// Loaded key counts. Zero means that file is missing.
struct CameraKeyCounts {
    int s01 = 0;
    int s02 = 0;
    int b01 = 0;
    int tv_ss = 0;
    int tv_bb = 0;
    int tv_bs = 0;
};

enum class CameraCueKind : std::uint8_t { Transition, Play, Hold, FallbackS01, Blend };

// One obstacle's time on the ribbon. The spin may play only while none of
// these intervals cover the clock.
struct CameraObstacle {
    std::int64_t hit_ms = 0;
    // On screen from hit_ms - approach_ms. Zero treats the obstacle as a point.
    std::int32_t approach_ms = 0;
    // First hit-window time, when the caller has the obstacle's play window.
    std::int64_t window_open_ms = 0;
    bool has_window = false;
};

// Primary looping-pan lengths. Shorter usable gaps may get a variable duration.
inline constexpr std::int64_t kLoopingPanShortMs = 4000;
inline constexpr std::int64_t kLoopingPanLongMs = 8000;
// Stage-start pans hold their opening pose this long before movement begins.
inline constexpr std::int64_t kLoopingPanStartDelayMs = 250;
// Leave the play camera back before the first hit-window instant.
inline constexpr std::int64_t kCameraPanWindowLeadMs = 100;
// Existing transition minimum for section-boundary spins.
inline constexpr std::int64_t kShortTransitionMinMs = 4000;
// Section-boundary transitions still use this lead time.
inline constexpr std::int64_t kCameraInputLeadMs = 1000;
// Skipped handoff. Long enough to hide a pose pop, short of a spin.
inline constexpr std::int64_t kCameraHandoffMs = 400;

// Which file and key the disc camera samples at one audio time.
struct CameraCue {
    CameraCueKind kind = CameraCueKind::FallbackS01;
    RoadCamera road = RoadCamera::S01;
    TransitionCamera transition = TransitionCamera::TvSs;
    float key_index = 0.f;
    int section = 0;
    // A transition is inside its 30 keys/s window. Before that window the
    // first key is held and this stays false.
    bool playing = false;
    // The wanted file had no keys, so the cue is held S01 at key 0.
    bool file_missing = false;
    // Blend only. 0 is the previous section's last key, 1 is this play key.
    float blend = 0.f;
    RoadCamera blend_road = RoadCamera::S01;
    float blend_key = 0.f;
};

// Road file for section `section` of course `course_index` (0 is course 1).
// An index outside 0..5 uses S01. A section past the third uses the third entry.
[[nodiscard]] RoadCamera course_section_camera(int course_index, int section);

// True when that table entry was not in the courses 1–5 capture.
// Course 5 sections 2 and 3 are guesses. Course 6 is entirely a placeholder.
[[nodiscard]] bool course_section_camera_is_guess(int course_index, int section);

// Transition that ends when section `section` starts.
[[nodiscard]] TransitionCamera transition_into_section(int course_index, int section);

// Key 0 at `start_ms`, last key at `end_ms`. The timer is key × kAncTimerScale.
// One key, or a non-positive span, stays on key 0.
[[nodiscard]] float stretched_key_index(std::int64_t time_ms,
                                        std::int64_t start_ms,
                                        std::int64_t end_ms,
                                        int key_count);

// 240 keys at 30 per second is 8000 ms. Zero keys is 0.
[[nodiscard]] std::int64_t transition_duration_ms(int key_count);

// Schedule at `audio_ms` (course audio head; negative is the prelude).
// The in-play file is stretched across each span. After `end_ms` the last key
// is held until the transition into the next section. The intro is the
// transition into section 1. A missing file reports FallbackS01.
// The looping pan always plays at stage start. It also plays through an
// obstacle-free gap of at least `kShortTransitionMinMs`. The 240 keys are
// stretched or compressed to fill that gap, and the pan finishes at least
// `kCameraInputLeadMs` before the next obstacle needs input. With no obstacles,
// each boundary keeps the watched spin: 30 keys per second, ending as the next
// section starts. A shorter gap holds the previous section's last key and
// blends into the next play key. `gate_transitions` is accepted for callers;
// it does not suppress the stage-start pan.
[[nodiscard]] CameraCue schedule_disc_camera(int course_index,
                                             std::int64_t audio_ms,
                                             std::span<const CameraSectionSpan> sections,
                                             CameraKeyCounts keys,
                                             std::span<const CameraObstacle> obstacles = {},
                                             bool gate_transitions = false);

// No section spans: hold S01 at key 0. The stage-start pan always plays when
// there are obstacles, fitted so it ends at least `kCameraInputLeadMs` before
// the first hit. A later obstacle-free gap of at least `kShortTransitionMinMs`
// stretches or compresses TV_SS across that gap, with the same lead. An empty
// obstacle list leaves the prelude intro to the caller. `audio_end_ms` 0 skips
// the tail after the last hit.
[[nodiscard]] CameraCue schedule_held_gap_camera(std::int64_t audio_ms,
                                                 CameraKeyCounts keys,
                                                 std::span<const CameraObstacle> obstacles,
                                                 std::int64_t audio_end_ms);

// Playable pattern segments (types 1 and 2), in start order. A segment ends at
// the next segment's start, break or otherwise. The last one ends at
// `audio_duration_ms`, or `kTailMsWithoutAudio` after its start when that
// length is 0. Times use the same rounding as the course mapper.
[[nodiscard]] std::vector<CameraSectionSpan>
camera_sections_from_patterns(std::span<const FslPatternSegment> segments,
                              std::int32_t audio_duration_ms,
                              double units_per_second);

// Equal slices of a custom track, covering `[0, duration_ms]`. Three when each
// slice is at least about 20 s; otherwise two, or one. A non-positive duration
// returns no spans. The last slice absorbs any leftover millisecond.
[[nodiscard]] std::vector<CameraSectionSpan> custom_music_camera_sections(std::int32_t duration_ms);

// Gold courses only (5 and 6). Estimates from video, easy to tune. Not a RAM
// measurement. Screen y in the 512×286 frame grows downward, so the ribbon
// offset is negative while the ribbon is up. The figure steps right and stays.
inline constexpr std::int64_t kGoldRibbonRiseStartMs = 60000;
inline constexpr std::int64_t kGoldRibbonRiseMs = 2000;
inline constexpr std::int64_t kGoldRibbonHoldMs = 8000;
inline constexpr std::int64_t kGoldRibbonFallMs = 2000;
inline constexpr float kGoldRibbonRisePs = -22.f;
inline constexpr std::int64_t kGoldFigureShiftStartMs = 64000;
inline constexpr std::int64_t kGoldFigureShiftMs = 2500;
inline constexpr float kGoldFigureShiftPs = 32.f;

struct GoldShiftPs {
    // 512×286 frame. Negative ribbon_dy is up. Positive figure_dx is right.
    float ribbon_dy = 0.f;
    float figure_dx = 0.f;
};

[[nodiscard]] GoldShiftPs gold_shift_ps(std::int64_t audio_ms);

} // namespace oscilline
