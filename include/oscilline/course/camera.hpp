// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Built-in camera and disc-camera projection into the view.

#pragma once

#include "oscilline/anc.hpp"
#include "oscilline/course/camera_schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oscilline {

// PAL cue-to-music interval measured across six course starts; see docs/level-start-timing.md.
inline constexpr int kCourseStartDelayMs = 8000;

// Intro swings from a tilt and a centered figure into the play framing.
inline constexpr int kCameraIntroMs = 4000;
inline constexpr int kCameraOutroMs = 3000;
inline constexpr float kCameraIntroTiltDeg = 25.f;
// The figure and the hit line sit this far across the screen from the left.
inline constexpr float kHitXFraction = 0.12f;
// Intro starts with the figure near the middle, then pans to the hit line.
inline constexpr float kCameraIntroCenterFraction = 0.5f;
// Obstacles enter at the right edge.
inline constexpr float kSpawnXFraction = 1.f;
// A loop is about 15% of the screen width. Pairs are a bit wider.
inline constexpr float kLoopWidthFraction = 0.15f;
inline constexpr float kPairWidthScale = 1.35f;
// Ribbon vibration from the onset envelope: ±1.5% of screen height, ~100 ms decay.
inline constexpr float kRibbonVibrateFraction = 0.015f;
inline constexpr int kRibbonVibrateDecayMs = 100;
inline constexpr float kRibbonVibrateHz = 12.f;
// Onset strength that maps to the full vibration amplitude.
inline constexpr float kRibbonOnsetKnee = 8.f;

enum class CameraPhase : std::uint8_t { Intro, Play, Outro };

// Disc-camera clock. 30 keys per second from course start, not from the ANC
// header and not from song time. A 240-key intro lasts 8.0 s.
inline constexpr int kDiscCameraKeysPerSecond = 30;

// PAL frame the disc projection is stated in, center (256, 124). x is not
// mirrored: camera-right is down x forward, as on the GTE. Emulator captures of
// the course screen are 512x286, and that frame stretches onto the logical
// 4:3 view.
inline constexpr float kDiscBufferWidth = 512.f;
inline constexpr float kDiscBufferHeight = 286.f;
inline constexpr float kDiscCenterX = 256.f;
inline constexpr float kDiscCenterY = 124.f;
// Intro zoom relative to the play camera while the ribbon is turned. A framed
// dolly onto the origin is not clamped; see gameplay_camera.
inline constexpr float kDiscIntroZoomMin = 0.8f;
inline constexpr float kDiscIntroZoomMax = 1.3f;

struct CameraPose {
    CameraPhase phase = CameraPhase::Play;
    float tilt_deg = 0.f;
    // Screen x of the figure and the hit line.
    float figure_x = 0.f;
    // Disc projection only. The built-in path leaves `disc` false and ignores these.
    // `zoom` is apparent size divided by the play camera's apparent size.
    float figure_y = 0.f;
    float zoom = 1.f;
    bool disc = false;
};

// Road files. `intro` is ROAD/TV_SS.ANC and `play` is ROAD/S01.ANC.
// S02, B01, TV_BB, and TV_BS are the other section files. With `sections`
// empty the intro plays from course start at 30 keys/s and play holds key 0.
// With obstacles, the stage-start pan always plays and later spins fill quiet
// gaps. With section spans, schedule_disc_camera picks the file and the key.
// `course_index` is 0 for course 1. `gold` adds the Gold screen shift.
struct DiscCameraPaths {
    const AncFile* intro = nullptr;
    const AncFile* play = nullptr;
    const AncFile* s02 = nullptr;
    const AncFile* b01 = nullptr;
    const AncFile* tv_bb = nullptr;
    const AncFile* tv_bs = nullptr;
    int course_index = -1;
    std::span<const CameraSectionSpan> sections{};
    std::span<const CameraObstacle> obstacles{};
    // Kept for callers. The stage-start pan plays on every course; later pans
    // fill quiet gaps. An empty obstacle list still uses the fixed prelude intro.
    bool gate_transitions = false;
    // Track length. 0 leaves the tail after the last hit without a spin.
    std::int64_t audio_end_ms = 0;
    bool gold = false;
};

// The ANC sample `gameplay_camera` projects at this instant.
struct DiscCameraSample {
    bool valid = false;
    // Intro file, including the hold on its last key when play is absent.
    bool from_intro = false;
    // Still inside the intro's key span. The phase is Intro only then.
    bool intro_phase = false;
    float key_index = 0.f;
    AncSample sample{};
    // File `sample` was taken from. Null when the cue is not valid.
    const AncFile* file = nullptr;
};

// Empty when neither file has keys. The intro/play choice matches gameplay_camera.
[[nodiscard]] DiscCameraSample
disc_camera_sample(std::int64_t now_ms, bool track_clock, DiscCameraPaths disc);

// `play_end_ms` is the course duration. The outro starts there and runs
// kCameraOutroMs. With track_clock, negative track time is the prelude and
// the camera reaches play framing at track zero. Play is static: no tilt and the figure at the hit
// line.
[[nodiscard]] CameraPose camera_at(std::int64_t now_ms,
                                   std::int32_t play_end_ms,
                                   float screen_width,
                                   bool track_clock = false);

// Key index on the disc-camera clock: elapsed_ms * 30 / 1000.
// Negative elapsed is 0. This is not a song position.
[[nodiscard]] float disc_camera_key_index(std::int64_t elapsed_ms);

// True while `elapsed_ms` is still inside an intro of `key_count` keys.
// The path ends at key_count / 30 seconds: elapsed * 30 < key_count * 1000.
[[nodiscard]] bool disc_camera_in_intro(std::int64_t elapsed_ms, std::size_t key_count);

// Elapsed real time from course start. With the track clock, course start is
// kCourseStartDelayMs before track zero. Negative results clamp to 0.
[[nodiscard]] std::int64_t disc_camera_elapsed_ms(std::int64_t now_ms, bool track_clock);

// Project one ANC sample. The figure point here is the world origin. The buffer
// center is (256, 124), and fov is the projection distance H:
//   ps_x = 256 + camera_x * H / z
//   ps_y = 124 + camera_y * H / z
// That buffer stretches onto `screen_width` x `screen_height`.
// Tilt is the screen angle of the ribbon (world +Z through the origin) after
// the look basis and the roll channel, in clockwise-positive degrees, wrapped
// to about ±180. Positive roll is counter-clockwise (a negative tilt). A
// world-up basis does not barrel-roll from pitch or yaw; the intro's full turn
// is this road angle as the eye orbits, unwrapped by gameplay_camera. No ROAD
// file sets the roll channel; a non-zero roll is still applied.
// `pixels_per_unit` is H / depth of the origin. `ok` is false when the origin
// cannot be projected.
struct DiscCameraFrame {
    bool ok = false;
    float figure_x = 0.f;
    float figure_y = 0.f;
    float tilt_deg = 0.f;
    float pixels_per_unit = 0.f;
};

[[nodiscard]] DiscCameraFrame
frame_disc_camera(const AncSample& sample, float screen_width, float screen_height);

// Disc pose when `use_disc_path` is set. With no section spans, `disc.intro`
// plays on the real-time clock above and then the first key of `disc.play` is
// held. With spans, each section stretches its road file from key 0 to the last
// key and holds that key. The looping pan always plays at stage start, and
// again in an obstacle-free gap of at least 4 s. That clip stretches or
// compresses to fill the gap and finishes at least 1 s before the next obstacle
// needs input. With no obstacles, the watched spin still ends as the next
// section starts. A short gap holds the previous key and blends into the next
// play key. A transition's tilt is the
// unwrapped road angle. While that ribbon is turned, the figure eases to the
// horizontal center at the play ribbon height and the zoom stays near the play
// scale, because the world origin is not the character the eye is circling.
// `play_end_ms` does not move this pose. `disc.gold` then adds the Gold shift.
// With the flag clear, or with no usable file, the result is `camera_at`.
[[nodiscard]] CameraPose gameplay_camera(std::int64_t now_ms,
                                         std::int32_t play_end_ms,
                                         float screen_width,
                                         float screen_height,
                                         bool track_clock,
                                         DiscCameraPaths disc,
                                         bool use_disc_path);

// Screen x of an obstacle that reaches `hit_x` at `hit_ms` and crosses from
// `spawn_x` in `scroll_approach_ms`.
[[nodiscard]] float obstacle_screen_x(float hit_x,
                                      float spawn_x,
                                      std::int32_t scroll_approach_ms,
                                      std::int32_t hit_ms,
                                      std::int64_t now_ms);

// Onset shake. The play picture does not call this: segmented ribbon jitter
// replaced the whole-stage translation. Empty audio, or a silent envelope, returns 0.
[[nodiscard]] float
ribbon_vibrate_px(std::span<const float> envelope, std::int64_t now_ms, float screen_height);

} // namespace oscilline
