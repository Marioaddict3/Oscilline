// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Windows, form changes, and the score multipliers.

#include "oscilline/course/play.hpp"

#include "oscilline/course/camera.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/score.hpp"
#include "oscilline/course/shapes.hpp"
#include "oscilline/render/viewport.hpp"

#include <algorithm>
#include <cmath>

namespace oscilline {
namespace {

constexpr float kPi = 3.14159265f;

float game_area_height(float screen_h) {
    if (std::isfinite(screen_h) && screen_h > 0.f) {
        return screen_h;
    }
    return static_cast<float>(kLogicalHeight);
}

struct SlotPoint {
    float cx = 0.f;
    float cy = 0.f;
    float x = 0.f;
    float y = 0.f;
    float radius = 0.f;
    float dir_x = 0.f;
    float dir_y = -1.f;
};

SlotPoint slot_point(float center_x, float center_y, float screen_h, int index) {
    if (index < 0) {
        index = 0;
    }
    const float height = game_area_height(screen_h);
    const float rx = kStreakRadiusXFraction * height;
    const float ry = kStreakRadiusYFraction * height;
    const float angle = (90.f + static_cast<float>(index) * kStreakStepDeg) * kPi / 180.f;
    const float ex = rx * std::cos(angle);
    const float ey = -ry * std::sin(angle);
    SlotPoint point;
    point.cx = center_x;
    point.cy = center_y;
    point.x = center_x + ex;
    point.y = center_y + ey;
    point.radius = std::hypot(ex, ey);
    if (point.radius > 1.e-6f) {
        point.dir_x = ex / point.radius;
        point.dir_y = ey / point.radius;
    }
    return point;
}

void begin_super_transform(PlayState& state, std::int64_t now_ms) {
    state.super_transform.start_ms = now_ms;
    state.super_transform.until_ms = now_ms + kSuperTransformMs;
    state.super_transform.sfx_id = kSuperTransformSfxId;
    state.super_transform.clip_id = kSuperTransformClipId;
}

void drop_form(PlayState& state) {
    if (state.form == Form::Super) {
        state.form = Form::Rabbit;
        state.super_transform = {};
    } else if (state.form == Form::Rabbit) {
        state.form = Form::Frog;
    } else if (state.form == Form::Frog) {
        state.form = Form::Worm;
    } else {
        state.form = Form::Out;
        state.finished = true;
    }
}

// True only when this call starts the rabbit → super effect.
bool rise_form(PlayState& state, std::int64_t now_ms) {
    if (state.form == Form::Worm) {
        state.form = Form::Frog;
    } else if (state.form == Form::Frog) {
        state.form = Form::Rabbit;
    } else if (state.form == Form::Rabbit) {
        state.form = Form::Super;
        begin_super_transform(state, now_ms);
        return true;
    }
    return false;
}

void note_hit(std::vector<PlayHit>* hits,
              std::uint8_t obstacle,
              Judgment judgment,
              Form before,
              Form after,
              bool super_transform) {
    if (hits == nullptr) {
        return;
    }
    PlayHit hit;
    hit.obstacle = obstacle;
    hit.judgment = judgment;
    hit.form_changed = before != after;
    hit.super_transform = super_transform;
    hits->push_back(hit);
}

void mark_miss(PlayState& state,
               std::int64_t now_ms,
               std::uint8_t obstacle,
               std::vector<PlayHit>* hits) {
    const Form before = state.form;
    state.last = Judgment::Miss;
    state.judged_ms = now_ms;
    state.gathered = 0;
    state.clear_run = 0;
    state.score_streak = 0;
    state.freestyle = 0;
    ++state.misses;
    ++state.damage;
    if (state.form != Form::Out && state.damage >= form_miss_limit(state.form)) {
        state.damage = 0;
        drop_form(state);
        state.burst_until_ms = now_ms + kFormBurstMs;
    }
    if (before != state.form) {
        state.hits_since_form = 0;
        state.last_hit_ms = -1;
    } else if (state.form != Form::Out) {
        ++state.hits_since_form;
        state.last_hit_ms = now_ms;
    }
    ++state.event_index;
    note_hit(hits, obstacle, Judgment::Miss, before, state.form, false);
}

void mark_clear(PlayState& state,
                std::int64_t now_ms,
                const ObstacleWindow& window,
                std::uint8_t obstacle,
                std::vector<PlayHit>* hits,
                std::int64_t judged_ms) {
    const Form before = state.form;
    // Perfect follows the calibrated press. The popup clock stays on the audio time.
    const bool perfect = judged_ms >= window.perfect_open && judged_ms <= window.perfect_close;
    state.score_streak = std::min(state.score_streak + 1, kScoreStreakCap);
    const int points = clear_score_points(
        state.score_streak, before, perfect ? Judgment::Perfect : Judgment::Good, state.freestyle);
    state.earned_score = std::min(state.earned_score, kMaxEarnedScore - points) + points;
    state.score = std::min(kMaxCouponScore - state.coupon_prior, state.score + points);
    state.freestyle = 0;
    if (perfect) {
        state.last = Judgment::Perfect;
        ++state.perfects;
    } else {
        state.last = Judgment::Good;
        ++state.goods;
    }
    state.judged_ms = now_ms;
    state.gathered = 0;
    state.damage = 0;
    ++state.clear_run;
    bool super_transform = false;
    if (state.clear_run >= kClearsToRise) {
        state.clear_run = 0;
        super_transform = rise_form(state, now_ms);
    }
    if (before != state.form) {
        state.hits_since_form = 0;
        state.last_hit_ms = -1;
    }
    ++state.event_index;
    note_hit(hits, obstacle, state.last, before, state.form, super_transform);
}

} // namespace

ObstacleWindow obstacle_window(const CourseEvent& event) {
    ObstacleWindow window;
    const std::int64_t hit = event.hit_ms;
    const int margin = kGoodWindowMs - kPerfectWindowMs;
    const int width = kPerfectWindowMs * 2;
    const std::uint8_t actions = obstacle_actions(event.obstacle);
    const bool loop_pair = obstacle_is_pair(event.obstacle) && (actions & kActionLoop) != 0 &&
                           (actions & kActionBlock) == 0;
    const std::uint8_t zone_obstacle = loop_pair ? kLoopObstacleId : event.obstacle;
    // Loop pairs use the plain loop's zone, offset to their own cross, so the
    // zone sits in the same place relative to the cross.
    std::int64_t front = hit;

    const std::int32_t travel = stage_scroll_approach_ms(event);
    const float screen_w = static_cast<float>(logical_width());
    const float span = screen_w * (kSpawnXFraction - kHitXFraction);
    const float obstacle_width_px =
        std::min(screen_w, static_cast<float>(kLogicalWidth)) * kLoopWidthFraction;
    const float leading =
        obstacle_leading_x(zone_obstacle, obstacle_width_px) +
        (loop_pair ? obstacle_loop_foot_x(event.obstacle, obstacle_width_px) : 0.f);
    if (travel > 0 && span > 0.f && std::isfinite(leading)) {
        const double px_per_ms = static_cast<double>(span) / static_cast<double>(travel);
        front += static_cast<std::int64_t>(std::llround(static_cast<double>(leading) / px_per_ms));
    }
    front += kJudgmentLateBiasMs;
    if ((actions & kActionLoop) != 0) {
        front += kLoopPerfectZoneShiftMs;
    }
    window.perfect_close = front;
    window.perfect_open = front - width;
    window.good_open = window.perfect_open - margin;
    window.good_close = window.perfect_close + kGoodIntoObstacleMs;
    return window;
}

PlayAdvanceResult play_advance(PlayState& state,
                               const CourseTimeline& course,
                               std::int64_t prev_ms,
                               std::int64_t now_ms,
                               std::uint8_t edges,
                               std::vector<PlayHit>* hits,
                               int timing_offset_ms) {
    PlayAdvanceResult result;
    if (state.paused || state.finished) {
        return result;
    }
    if (now_ms < prev_ms) {
        return result;
    }

    const bool armed = state.press_after_crash;
    const std::uint8_t incoming = edges;
    // Zero offset keeps the press on the audio clock, same as before calibration.
    const std::int64_t press_ms =
        edges != 0 ? calibrated_press_ms(now_ms, timing_offset_ms) : now_ms;
    bool applied = false;
    bool crashed = false;

    while (!state.finished && state.event_index < static_cast<int>(course.events.size())) {
        const CourseEvent& event = course.events[static_cast<std::size_t>(state.event_index)];
        const ObstacleWindow window = obstacle_window(event);
        const std::int32_t travel = stage_scroll_approach_ms(event);
        // The music has stopped. Anything still unmet is not a miss.
        if (obstacle_after_audio(event.hit_ms, travel, course.audio_end_ms) ||
            (course.audio_end_ms > 0 && now_ms >= course.audio_end_ms &&
             window.good_close > course.audio_end_ms)) {
            state.gathered = 0;
            ++state.event_index;
            continue;
        }
        const bool press_inside =
            edges != 0 && press_ms >= window.good_open && press_ms <= window.good_close;
        if (press_inside) {
            applied = true;
            state.gathered = static_cast<std::uint8_t>(state.gathered | edges);
            const std::uint8_t required = obstacle_actions(event.obstacle);
            const std::uint8_t extra =
                static_cast<std::uint8_t>(state.gathered & static_cast<std::uint8_t>(~required));
            edges = 0;
            if (extra != 0) {
                result.wrong = true;
                mark_miss(state, now_ms, event.obstacle, hits);
                crashed = true;
                continue;
            }
            if (required != 0 && state.gathered == required) {
                mark_clear(state, now_ms, window, event.obstacle, hits, press_ms);
                continue;
            }
            break;
        }
        if (now_ms < window.good_open) {
            break;
        }
        if (now_ms <= window.good_close) {
            break;
        }
        edges = 0;
        mark_miss(state, now_ms, event.obstacle, hits);
        crashed = true;
    }

    // A press that missed the window and did not resolve an obstacle is empty.
    // A press in the same step as a miss stays with that crash, not a whiff.
    if (incoming != 0 && !applied && !crashed) {
        result.whiff = true;
        // Freestyle only before an upcoming window, never inside it or after the last hit.
        if (state.event_index < static_cast<int>(course.events.size())) {
            const CourseEvent& next = course.events[static_cast<std::size_t>(state.event_index)];
            if (press_ms < obstacle_window(next).good_open) {
                const auto optional = static_cast<std::uint8_t>(~obstacle_actions(next.obstacle));
                state.freestyle =
                    static_cast<std::uint8_t>(state.freestyle | (incoming & optional & 15));
            }
        }
    }
    if (incoming != 0 && armed) {
        result.post_crash_press = true;
        state.press_after_crash = false;
    }
    if (crashed) {
        state.press_after_crash = true;
    }

    if (state.finished || now_ms < course.duration_ms) {
        return result;
    }
    if (state.event_index >= static_cast<int>(course.events.size())) {
        state.finished = true;
        return result;
    }
    const CourseEvent& pending = course.events[static_cast<std::size_t>(state.event_index)];
    if (now_ms < obstacle_window(pending).good_open) {
        state.finished = true;
    }
    return result;
}

std::string_view form_name(Form form) {
    switch (form) {
    case Form::Super:
        return "super";
    case Form::Rabbit:
        return "rabbit";
    case Form::Frog:
        return "frog";
    case Form::Worm:
        return "worm";
    case Form::Out:
        return "out";
    }
    return "rabbit";
}

StreakDot streak_ring_dot(float figure_x, float ribbon_y, float screen_h, int index) {
    const float height = game_area_height(screen_h);
    const float cy = ribbon_y - kStreakCenterAboveRibbon * height;
    const SlotPoint slot = slot_point(figure_x, cy, height, index);
    StreakDot dot;
    dot.x = slot.x;
    dot.y = slot.y;
    return dot;
}

float super_burst_u(std::int64_t start_ms, std::int64_t now_ms) {
    const float span = static_cast<float>(std::max(kSuperTransformMs, 1));
    const float u = static_cast<float>(now_ms - start_ms) / span;
    if (!std::isfinite(u)) {
        return 0.f;
    }
    return std::clamp(u, 0.f, 1.f);
}

SuperDash super_burst_dash(float center_x, float center_y, float screen_h, int slot, float u) {
    if (!std::isfinite(u)) {
        u = 1.f;
    }
    u = std::clamp(u, 0.f, 1.f);
    const SlotPoint point = slot_point(center_x, center_y, screen_h, slot);
    const float grown = 1.f - (1.f - u) * (1.f - u);
    const float radius = point.radius * (1.f + kSuperBurstGrowth * grown);
    const float length = kSuperDashLengthFraction * radius;
    SuperDash dash;
    dash.radius = radius;
    dash.length = length;
    dash.x0 = center_x + point.dir_x * (radius - 0.5f * length);
    dash.y0 = center_y + point.dir_y * (radius - 0.5f * length);
    dash.x1 = center_x + point.dir_x * (radius + 0.5f * length);
    dash.y1 = center_y + point.dir_y * (radius + 0.5f * length);
    return dash;
}

std::string_view judgment_name(Judgment judgment) {
    switch (judgment) {
    case Judgment::None:
        return "";
    case Judgment::Perfect:
        return "PERFECT";
    case Judgment::Good:
        return "GOOD";
    case Judgment::Miss:
        return "MISS";
    }
    return "";
}

} // namespace oscilline
