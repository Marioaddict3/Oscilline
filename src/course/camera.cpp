// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Built-in pan and the disc camera, including intro placement.

#include "oscilline/course/camera.hpp"

#include "oscilline/anc.hpp"
#include "oscilline/course/beats.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oscilline {
namespace {

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const float len = length(v);
    if (!(len > 1.e-8f) || !std::isfinite(len)) {
        return {};
    }
    return v * (1.f / len);
}

} // namespace

CameraPose
camera_at(std::int64_t now_ms, std::int32_t play_end_ms, float screen_width, bool track_clock) {
    const std::int64_t intro_lead = track_clock ? kCameraIntroMs : 0;
    now_ms += intro_lead;
    if (now_ms < 0) {
        now_ms = 0;
    }
    if (!(screen_width > 0.f)) {
        screen_width = 1.f;
    }
    const float hit_x = screen_width * kHitXFraction;
    const float center_x = screen_width * kCameraIntroCenterFraction;
    const std::int64_t play_end = std::max<std::int64_t>(play_end_ms, 0) + intro_lead;

    CameraPose pose;
    pose.figure_x = hit_x;
    if (now_ms < play_end && now_ms < kCameraIntroMs) {
        const float u = static_cast<float>(now_ms) / static_cast<float>(kCameraIntroMs);
        pose.phase = CameraPhase::Intro;
        pose.tilt_deg = lerp(kCameraIntroTiltDeg, 0.f, u);
        pose.figure_x = lerp(center_x, hit_x, u);
        return pose;
    }
    if (now_ms < play_end) {
        pose.phase = CameraPhase::Play;
        pose.tilt_deg = 0.f;
        pose.figure_x = hit_x;
        return pose;
    }
    float u = 1.f;
    if (kCameraOutroMs > 0) {
        u = static_cast<float>(now_ms - play_end) / static_cast<float>(kCameraOutroMs);
    }
    u = std::clamp(u, 0.f, 1.f);
    pose.phase = CameraPhase::Outro;
    pose.tilt_deg = lerp(0.f, kCameraIntroTiltDeg, u);
    pose.figure_x = lerp(hit_x, center_x, u);
    return pose;
}

float disc_camera_key_index(std::int64_t elapsed_ms) {
    if (elapsed_ms <= 0) {
        return 0.f;
    }
    return static_cast<float>(elapsed_ms) * static_cast<float>(kDiscCameraKeysPerSecond) / 1000.f;
}

bool disc_camera_in_intro(std::int64_t elapsed_ms, std::size_t key_count) {
    if (key_count == 0 || elapsed_ms < 0) {
        return false;
    }
    const auto keys = static_cast<std::int64_t>(key_count);
    return elapsed_ms * static_cast<std::int64_t>(kDiscCameraKeysPerSecond) < keys * 1000;
}

std::int64_t disc_camera_elapsed_ms(std::int64_t now_ms, bool track_clock) {
    std::int64_t elapsed =
        track_clock ? now_ms + static_cast<std::int64_t>(kCourseStartDelayMs) : now_ms;
    if (elapsed < 0) {
        elapsed = 0;
    }
    return elapsed;
}

namespace {

// Origin projection plus the ribbon angle. The look can be valid when the origin
// is behind the eye; that is how an orbit passes the world origin without the
// character being there.
struct DiscProjection {
    bool look_ok = false;
    bool origin_ok = false;
    bool tilt_valid = false;
    float tilt_deg = 0.f;
    float figure_x = 0.f;
    float figure_y = 0.f;
    float pixels_per_unit = 0.f;
    float depth = 0.f;
    float lateral = 0.f;
    float look_distance = 0.f;
};

// Lateral/depth below this means the origin lies on the look ray, so a dolly
// toward it is the character. S01 sits near 0.4 and is not on-axis.
constexpr float kOnAxisLateral = 0.2f;
// Origin screen y outside this band is not the character. The play ribbon is
// near 0.67; the buffer center is near 0.43.
constexpr float kPlayfieldMinFraction = 0.15f;
constexpr float kPlayfieldMaxFraction = 0.78f;

DiscProjection
project_disc_camera(const AncSample& sample, float screen_width, float screen_height) {
    DiscProjection projected;
    if (!(screen_width > 0.f)) {
        screen_width = 1.f;
    }
    if (!(screen_height > 0.f)) {
        screen_height = 1.f;
    }
    const Vec3 eye{sample.eye_x, sample.eye_y, sample.eye_z};
    const Vec3 target{sample.target_x, sample.target_y, sample.target_z};
    const Vec3 look = target - eye;
    projected.look_distance = length(look);
    const Vec3 forward = normalized(look);
    if (forward.x == 0.f && forward.y == 0.f && forward.z == 0.f) {
        return projected;
    }
    // PS +Y is down. right = down × forward, so an unrolled +Z look has +X to
    // camera-right and screen-right.
    Vec3 right = cross(Vec3{0.f, 1.f, 0.f}, forward);
    if (!(length(right) > 1.e-6f)) {
        right = cross(Vec3{0.f, 0.f, 1.f}, forward);
    }
    right = normalized(right);
    if (right.x == 0.f && right.y == 0.f && right.z == 0.f) {
        return projected;
    }
    const Vec3 down = normalized(cross(forward, right));
    const float roll = sample.roll * (std::numbers::pi_v<float> * 2.f / 4096.f);
    const float cosine = std::cos(roll);
    const float sine = std::sin(roll);
    // Positive roll swings the road (world +Z) toward screen-up: counter-clockwise.
    const Vec3 rolled_right = right * cosine + down * sine;
    const Vec3 rolled_down = down * cosine - right * sine;
    projected.look_ok = projected.look_distance > 1.e-3f && sample.fov > 0.f &&
                        std::isfinite(sample.fov) && std::isfinite(projected.look_distance);

    // The ribbon is world +Z: S01 looks along -X and sees it level, with +Z to
    // screen-right. Roll is already in the basis, so a non-zero roll
    // channel adds to this angle. Pitch and yaw change it only when the road
    // is no longer camera-horizontal, which is the intro orbit.
    const float road_x = rolled_right.z;
    const float road_y = rolled_down.z;
    if (road_x * road_x + road_y * road_y > 1.e-8f) {
        projected.tilt_deg = std::atan2(road_y, road_x) * (180.f / std::numbers::pi_v<float>);
        projected.tilt_valid = std::isfinite(projected.tilt_deg);
    }

    const Vec3 from_eye = Vec3{0.f, 0.f, 0.f} - eye;
    projected.depth = dot(from_eye, forward);
    if (!(projected.depth > 1.e-3f) || !projected.look_ok) {
        return projected;
    }
    const float cam_x = dot(from_eye, rolled_right);
    const float cam_y = dot(from_eye, rolled_down);
    projected.lateral = std::hypot(cam_x, cam_y);
    const float h = anc_projection_h(sample.fov);
    const float ps_x = kDiscCenterX + (cam_x * h) / projected.depth;
    const float ps_y = kDiscCenterY + (cam_y * h) / projected.depth;
    const float pixels = h / projected.depth;
    if (!std::isfinite(ps_x) || !std::isfinite(ps_y) || !std::isfinite(pixels) ||
        !std::isfinite(projected.lateral)) {
        return projected;
    }
    projected.origin_ok = true;
    projected.figure_x = ps_x * (screen_width / kDiscBufferWidth);
    projected.figure_y = ps_y * (screen_height / kDiscBufferHeight);
    projected.pixels_per_unit = pixels;
    return projected;
}

// Walk the intro so the road angle can pass ±180 and keep going to -360.
// An edge-on sample holds the previous angle. Screen size does not affect it.
float unwrapped_road_tilt(const AncFile& file, float key_index) {
    if (file.keys.empty()) {
        return 0.f;
    }
    const float last = static_cast<float>(file.keys.size() - 1);
    const float end = std::clamp(key_index, 0.f, last);
    constexpr float kStep = 0.25f;
    float accum = 0.f;
    float previous = 0.f;
    bool have = false;
    const auto consume = [&](float index) {
        const DiscProjection projected = project_disc_camera(sample_anc_at(file, index), 1.f, 1.f);
        if (!projected.tilt_valid) {
            return;
        }
        if (!have) {
            accum = projected.tilt_deg;
            previous = projected.tilt_deg;
            have = true;
            return;
        }
        float delta = projected.tilt_deg - previous;
        if (delta > 180.f) {
            delta -= 360.f;
        } else if (delta < -180.f) {
            delta += 360.f;
        }
        accum += delta;
        previous = projected.tilt_deg;
    };
    // An integer count avoids accumulating float error in the step.
    for (int step = 0; static_cast<float>(step) * kStep < end; ++step) {
        consume(static_cast<float>(step) * kStep);
    }
    consume(end);
    return have ? accum : 0.f;
}

bool origin_in_playfield(float x, float y, float screen_width, float screen_height) {
    return x >= 0.f && x <= screen_width && y >= screen_height * kPlayfieldMinFraction &&
           y <= screen_height * kPlayfieldMaxFraction;
}

// The intro figure eases from the level-ribbon anchor to the horizontal center
// as the road turns, and back as it finishes the circle. Its feet stay on the
// play ribbon height there: in PAL captures the turning ribbon passes the
// figure's feet near 0.63-0.74 of the height, not at the buffer center. The anchor is the origin
// when that point is still in the playfield. Otherwise it is the play pose.
struct IntroPlacement {
    float figure_x = 0.f;
    float figure_y = 0.f;
    float zoom = 1.f;
};

IntroPlacement place_intro_figure(const DiscProjection& projected,
                                  float tilt_deg,
                                  float origin_zoom,
                                  const DiscProjection& play,
                                  bool have_play,
                                  float screen_width,
                                  float screen_height,
                                  float fov) {
    const float center_x = kDiscCenterX * screen_width / kDiscBufferWidth;
    const float center_y = kDiscCenterY * screen_height / kDiscBufferHeight;
    const float from_level = std::fabs(std::remainder(tilt_deg, 360.f));
    const float toward_center = std::clamp(from_level / 90.f, 0.f, 1.f);
    const bool on_axis = projected.origin_ok && projected.depth > 1.e-3f &&
                         projected.lateral <= projected.depth * kOnAxisLateral;
    const bool framed =
        projected.origin_ok &&
        origin_in_playfield(projected.figure_x, projected.figure_y, screen_width, screen_height);

    float level_x = center_x;
    float level_y = center_y;
    float level_zoom = 1.f;
    if (framed) {
        level_x = projected.figure_x;
        level_y = projected.figure_y;
        level_zoom = origin_zoom;
        if (!on_axis) {
            level_zoom = std::clamp(level_zoom, kDiscIntroZoomMin, kDiscIntroZoomMax);
        }
    } else if (have_play && play.origin_ok) {
        level_x = play.figure_x;
        level_y = play.figure_y;
    }

    float spin_zoom = std::clamp(level_zoom, kDiscIntroZoomMin, kDiscIntroZoomMax);
    if (have_play && play.pixels_per_unit > 0.f && projected.look_distance > 1.e-3f && fov > 0.f &&
        std::isfinite(fov)) {
        // Raw fov channel, not anc_projection_h. The road divide uses H.
        const float look_zoom = (fov / projected.look_distance) / play.pixels_per_unit;
        if (std::isfinite(look_zoom) && look_zoom > 0.f) {
            spin_zoom = std::clamp(look_zoom, kDiscIntroZoomMin, kDiscIntroZoomMax);
        }
    }

    IntroPlacement placed;
    const float spin_y = have_play && play.origin_ok ? play.figure_y : center_y;
    placed.figure_x = lerp(level_x, center_x, toward_center);
    placed.figure_y = lerp(level_y, spin_y, toward_center);
    placed.zoom = lerp(level_zoom, spin_zoom, toward_center);
    return placed;
}

} // namespace

DiscCameraFrame
frame_disc_camera(const AncSample& sample, float screen_width, float screen_height) {
    const DiscProjection projected = project_disc_camera(sample, screen_width, screen_height);
    DiscCameraFrame frame;
    if (!projected.origin_ok) {
        return frame;
    }
    frame.ok = true;
    frame.figure_x = projected.figure_x;
    frame.figure_y = projected.figure_y;
    frame.tilt_deg = projected.tilt_valid ? projected.tilt_deg : 0.f;
    frame.pixels_per_unit = projected.pixels_per_unit;
    return frame;
}

namespace {

int key_count_of(const AncFile* file) {
    if (file == nullptr || file->keys.empty()) {
        return 0;
    }
    return static_cast<int>(file->keys.size());
}

const AncFile* road_file(const DiscCameraPaths& disc, RoadCamera road) {
    switch (road) {
    case RoadCamera::S02:
        return disc.s02;
    case RoadCamera::B01:
        return disc.b01;
    case RoadCamera::S01:
        return disc.play;
    }
    return disc.play;
}

const AncFile* transition_file(const DiscCameraPaths& disc, TransitionCamera transition) {
    switch (transition) {
    case TransitionCamera::TvBb:
        return disc.tv_bb;
    case TransitionCamera::TvBs:
        return disc.tv_bs;
    case TransitionCamera::TvSs:
        return disc.intro;
    }
    return disc.intro;
}

bool usable(const AncFile* file) {
    return file != nullptr && !file->keys.empty();
}

AncSample lerp_sample(const AncSample& from, const AncSample& to, float t) {
    if (!(t > 0.f)) {
        return from;
    }
    if (t >= 1.f) {
        return to;
    }
    const auto mix = [t](float a, float b) { return a + (b - a) * t; };
    AncSample sample;
    sample.eye_x = mix(from.eye_x, to.eye_x);
    sample.eye_y = mix(from.eye_y, to.eye_y);
    sample.eye_z = mix(from.eye_z, to.eye_z);
    sample.target_x = mix(from.target_x, to.target_x);
    sample.target_y = mix(from.target_y, to.target_y);
    sample.target_z = mix(from.target_z, to.target_z);
    sample.roll = mix(from.roll, to.roll);
    sample.fov = mix(from.fov, to.fov);
    return sample;
}

bool interval_blocked(std::int64_t lo, std::int64_t hi, std::span<const CameraObstacle> obstacles) {
    if (hi <= lo) {
        return false;
    }
    for (const CameraObstacle& obstacle : obstacles) {
        const std::int64_t approach = obstacle.approach_ms > 0 ? obstacle.approach_ms : 0;
        const std::int64_t spawn = obstacle.hit_ms - approach;
        if (spawn < hi && obstacle.hit_ms > lo) {
            return true;
        }
    }
    return false;
}

DiscCameraSample sample_from_cue(const DiscCameraPaths& disc, const CameraCue& cue) {
    DiscCameraSample picked;
    if (cue.kind == CameraCueKind::Blend) {
        const AncFile* from = road_file(disc, cue.blend_road);
        const AncFile* to = road_file(disc, cue.road);
        if (!usable(from)) {
            from = disc.play;
        }
        if (!usable(to)) {
            to = disc.play;
        }
        if (!usable(from) && !usable(to)) {
            return picked;
        }
        const AncSample source =
            usable(from) ? sample_anc_at(*from, cue.blend_key) : sample_anc_at(*to, 0.f);
        const AncSample dest = usable(to) ? sample_anc_at(*to, cue.key_index) : source;
        picked.sample = lerp_sample(source, dest, cue.blend);
        picked.key_index = cue.key_index;
        picked.file = usable(to) ? to : from;
        picked.valid = true;
        return picked;
    }
    const AncFile* file = nullptr;
    float key_index = cue.key_index;
    bool from_intro = false;
    if (cue.kind == CameraCueKind::Transition && !cue.file_missing) {
        file = transition_file(disc, cue.transition);
        from_intro = cue.playing;
    } else if ((cue.kind == CameraCueKind::Play || cue.kind == CameraCueKind::Hold) &&
               !cue.file_missing) {
        file = road_file(disc, cue.road);
    }
    if (!usable(file)) {
        file = disc.play;
        key_index = 0.f;
        from_intro = false;
    }
    if (!usable(file)) {
        return picked;
    }
    picked.key_index = key_index;
    picked.sample = sample_anc_at(*file, key_index);
    picked.file = file;
    picked.from_intro = from_intro;
    picked.intro_phase = from_intro;
    picked.valid = true;
    return picked;
}

} // namespace

DiscCameraSample disc_camera_sample(std::int64_t now_ms, bool track_clock, DiscCameraPaths disc) {
    DiscCameraSample picked;
    const bool have_intro = usable(disc.intro);
    const bool have_play = usable(disc.play);
    if (!disc.sections.empty()) {
        CameraKeyCounts keys;
        keys.s01 = key_count_of(disc.play);
        keys.s02 = key_count_of(disc.s02);
        keys.b01 = key_count_of(disc.b01);
        keys.tv_ss = key_count_of(disc.intro);
        keys.tv_bb = key_count_of(disc.tv_bb);
        keys.tv_bs = key_count_of(disc.tv_bs);
        const CameraCue cue = schedule_disc_camera(
            disc.course_index, now_ms, disc.sections, keys, disc.obstacles, disc.gate_transitions);
        return sample_from_cue(disc, cue);
    }
    if (disc.gate_transitions) {
        CameraKeyCounts keys;
        keys.s01 = key_count_of(disc.play);
        keys.tv_ss = key_count_of(disc.intro);
        const CameraCue cue =
            schedule_held_gap_camera(now_ms, keys, disc.obstacles, disc.audio_end_ms);
        // Obstacles fit the stage-start pan and later gaps. An empty chart keeps
        // the fixed prelude intro at 30 keys per second.
        if (!disc.obstacles.empty() || (cue.kind == CameraCueKind::Transition && cue.playing)) {
            return sample_from_cue(disc, cue);
        }
        const std::int64_t elapsed = disc_camera_elapsed_ms(now_ms, track_clock);
        const bool want_intro =
            have_intro && disc_camera_in_intro(elapsed, disc.intro->keys.size());
        const std::int64_t intro_lo =
            track_clock ? -static_cast<std::int64_t>(kCourseStartDelayMs) : 0;
        const std::int64_t intro_hi =
            intro_lo +
            transition_duration_ms(static_cast<int>(have_intro ? disc.intro->keys.size() : 0));
        if (want_intro && !interval_blocked(intro_lo, intro_hi, disc.obstacles)) {
            picked.key_index = disc_camera_key_index(elapsed);
            picked.sample = sample_anc_at(*disc.intro, picked.key_index);
            picked.file = disc.intro;
            picked.from_intro = true;
            picked.intro_phase = true;
            picked.valid = true;
            return picked;
        }
        return sample_from_cue(disc, cue);
    }
    if (!have_intro && !have_play) {
        return picked;
    }
    const std::int64_t elapsed = disc_camera_elapsed_ms(now_ms, track_clock);
    const bool in_intro = have_intro && disc_camera_in_intro(elapsed, disc.intro->keys.size());
    if (in_intro) {
        picked.key_index = disc_camera_key_index(elapsed);
        picked.sample = sample_anc_at(*disc.intro, picked.key_index);
        picked.file = disc.intro;
        picked.from_intro = true;
        picked.intro_phase = true;
    } else if (have_play) {
        // First key only. The play file is not walked, looped, or stretched.
        picked.sample = sample_anc_at(*disc.play, 0.f);
        picked.file = disc.play;
    } else {
        picked.key_index = static_cast<float>(disc.intro->keys.size() - 1);
        picked.sample = sample_anc_at(*disc.intro, picked.key_index);
        picked.file = disc.intro;
        picked.from_intro = true;
    }
    picked.valid = true;
    return picked;
}

CameraPose gameplay_camera(std::int64_t now_ms,
                           std::int32_t play_end_ms,
                           float screen_width,
                           float screen_height,
                           bool track_clock,
                           DiscCameraPaths disc,
                           bool use_disc_path) {
    const CameraPose base = camera_at(now_ms, play_end_ms, screen_width, track_clock);
    const auto finish = [&](CameraPose pose) {
        if (!disc.gold) {
            return pose;
        }
        const GoldShiftPs shift = gold_shift_ps(now_ms);
        const float width = screen_width > 0.f ? screen_width : 1.f;
        const float height = screen_height > 0.f ? screen_height : 1.f;
        pose.figure_x += shift.figure_dx * width / kDiscBufferWidth;
        pose.figure_y += shift.ribbon_dy * height / kDiscBufferHeight;
        return pose;
    };
    if (!use_disc_path) {
        return finish(base);
    }
    const DiscCameraSample picked = disc_camera_sample(now_ms, track_clock, disc);
    if (!picked.valid) {
        return finish(base);
    }
    const AncSample& sample = picked.sample;
    const bool from_intro = picked.from_intro;
    const float key_index = picked.key_index;
    const CameraPhase phase = picked.intro_phase ? CameraPhase::Intro : CameraPhase::Play;
    const bool have_play = disc.play != nullptr && !disc.play->keys.empty();
    const DiscProjection projected = project_disc_camera(sample, screen_width, screen_height);
    if (!projected.look_ok || (!from_intro && !projected.origin_ok)) {
        return finish(base);
    }
    DiscProjection play_projected;
    if (have_play) {
        play_projected =
            project_disc_camera(sample_anc_at(*disc.play, 0.f), screen_width, screen_height);
    }
    float zoom = 1.f;
    if (have_play && play_projected.origin_ok && play_projected.pixels_per_unit > 0.f &&
        projected.origin_ok) {
        zoom = projected.pixels_per_unit / play_projected.pixels_per_unit;
    }
    float tilt = projected.tilt_valid ? projected.tilt_deg : 0.f;
    float figure_x = projected.figure_x;
    float figure_y = projected.figure_y;
    if (from_intro && picked.file != nullptr) {
        tilt = unwrapped_road_tilt(*picked.file, key_index);
        const IntroPlacement placed = place_intro_figure(projected,
                                                         tilt,
                                                         zoom,
                                                         play_projected,
                                                         have_play && play_projected.origin_ok,
                                                         screen_width,
                                                         screen_height,
                                                         sample.fov);
        figure_x = placed.figure_x;
        figure_y = placed.figure_y;
        zoom = placed.zoom;
    }
    if (!std::isfinite(zoom) || !(zoom > 0.f) || !std::isfinite(tilt) || !std::isfinite(figure_x) ||
        !std::isfinite(figure_y)) {
        return finish(base);
    }
    CameraPose pose;
    pose.phase = phase;
    pose.tilt_deg = tilt;
    pose.figure_x = figure_x;
    pose.figure_y = figure_y;
    pose.zoom = zoom;
    pose.disc = true;
    return finish(pose);
}

float obstacle_screen_x(float hit_x,
                        float spawn_x,
                        std::int32_t scroll_approach_ms,
                        std::int32_t hit_ms,
                        std::int64_t now_ms) {
    if (scroll_approach_ms <= 0) {
        return hit_x;
    }
    const float px_per_ms = (spawn_x - hit_x) / static_cast<float>(scroll_approach_ms);
    return hit_x + static_cast<float>(hit_ms - now_ms) * px_per_ms;
}

float ribbon_vibrate_px(std::span<const float> envelope, std::int64_t now_ms, float screen_height) {
    if (envelope.empty() || now_ms < 0 || !(screen_height > 0.f)) {
        return 0.f;
    }
    const int index = static_cast<int>(now_ms / kOnsetStepMs);
    const int look = kRibbonVibrateDecayMs * 4 / kOnsetStepMs + 1;
    float amp = 0.f;
    for (int age_steps = 0; age_steps <= look; ++age_steps) {
        const int sample = index - age_steps;
        if (sample < 0 || static_cast<std::size_t>(sample) >= envelope.size()) {
            continue;
        }
        const float age_ms =
            static_cast<float>(now_ms - static_cast<std::int64_t>(sample) * kOnsetStepMs);
        if (age_ms < 0.f) {
            continue;
        }
        const float decay = std::exp(-age_ms / static_cast<float>(kRibbonVibrateDecayMs));
        amp = std::max(amp, envelope[static_cast<std::size_t>(sample)] * decay);
    }
    if (!(amp > 0.f)) {
        return 0.f;
    }
    const float norm = amp / (amp + kRibbonOnsetKnee);
    const float wave = std::sin(2.f * std::numbers::pi_v<float> * kRibbonVibrateHz *
                                static_cast<float>(now_ms) / 1000.f);
    return norm * kRibbonVibrateFraction * screen_height * wave;
}

} // namespace oscilline
