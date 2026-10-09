// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Judgment, scoring, forms, and the clear-streak ring.

#pragma once

#include "oscilline/audio/sfx.hpp"
#include "oscilline/course/mapping.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oscilline {

// Placeholder forms. These names are original; they are not a commercial character.
// Super sits above rabbit. Out is the failed run.
enum class Form : std::uint8_t { Super, Rabbit, Frog, Worm, Out };

enum class Judgment : std::uint8_t { None, Perfect, Good, Miss };

// Doubled in time to keep their on-screen span when obstacles scroll at half speed.
inline constexpr int kPerfectWindowMs = 100;
inline constexpr int kGoodWindowMs = 240;
// Feel-tuning offsets. The late bias is added to the instant the outline's front
// edge reaches Rabbit, and perfect ends at that biased instant.
inline constexpr int kJudgmentLateBiasMs = 20;
// Shift loop-related timing windows slightly later, moving their perfect zone right.
inline constexpr int kLoopPerfectZoneShiftMs = 20;
// Good extends the same margin after perfect as it has before perfect.
inline constexpr int kGoodIntoObstacleMs = kGoodWindowMs - kPerfectWindowMs;
// Accuracy bonuses added to the capped clear streak.
inline constexpr int kScorePerfect = 2;
inline constexpr int kScoreGood = 1;
inline constexpr int kScoreStreakCap = 11;
// Misses inside one form before it drops. Super has one hit of protection.
inline constexpr int kSuperMisses = 1;
inline constexpr int kRabbitMisses = 10;
inline constexpr int kFrogMisses = 6;
inline constexpr int kWormMisses = 3;
// Consecutive clears that raise one form on every rung: worm, frog, rabbit,
// super. Clears 1..17 each light one streak slot. The 18th clear promotes
// and draws no 18th dot. A miss sets the run back to 0. A clear also wipes
// the current damage. High: footage of the original, rabbit to super.
inline constexpr int kClearsToRise = 18;
// Scribble burst after a form drop. Hits during the burst are still judged.
// The rabbit → super rung does not use this burst.
inline constexpr int kFormBurstMs = 1000;
inline constexpr int kFormBurstStrokes = 16;
// Rabbit → super burst. t = 0 is the promoting clear. Every dash is gone
// at 0.28 s, with no fade. High.
inline constexpr int kSuperTransformMs = 280;
// GAME bank (PSJ_SE) VAG index 19, program 1 tone 14, 11025 Hz, one-shot.
// High. Resolved from the disc at playback; a missing bank stays silent.
inline constexpr int kSuperTransformSfxId = static_cast<int>(SfxId::SuperPromote);
// No separate transform clip. The promoting clear keeps the run pose and
// plays that clear's clip on the super model.
inline constexpr int kSuperTransformClipId = -1;
// Queen/Super doubles the streak award; other playable forms are equal.
inline constexpr int kSuperScoreMultiplier = 2;
inline constexpr int kRabbitScoreMultiplier = 1;
inline constexpr int kFrogScoreMultiplier = 1;
inline constexpr int kWormScoreMultiplier = 1;
// 512×286 PAL pixels to the 640×480 logical view.
inline constexpr float kPsToLogicalX = 1.25f;
inline constexpr float kPsToLogicalY = 1.678f;
// Clear-streak ring. H is the 4:3 game-area height. A near-circle centered on
// Rabbit: the center is `kStreakCenterAboveRibbon` of H above the ribbon line.
// Seventeen slots, 360/17 degrees (about 21.18). Slot 0 is 12 o'clock. Each
// next slot is counter-clockwise on screen. Clear k (1..17) shows dot k.
// Super form draws none. High, measured from footage of the original.
inline constexpr int kStreakSlotCount = 17;
inline constexpr float kStreakStepDeg = 360.f / static_cast<float>(kStreakSlotCount);
inline constexpr float kStreakRadiusXFraction = 0.133f;
inline constexpr float kStreakRadiusYFraction = 0.145f;
inline constexpr float kStreakCenterAboveRibbon = 0.104f;
inline constexpr float kStreakDotPs = 2.f;
inline constexpr int kStreakDotR = 208;
inline constexpr int kStreakDotG = 88;
inline constexpr int kStreakDotB = 176;
// Radial dash that replaces each of the 17 dots on the promoting frame.
// r = R0 * (1 + growth * (1 - (1 - u)^2)), u = t / 0.28 s, length = 0.16 * r.
// About 0.02 H growing to about 0.10 H. Color is low confidence.
inline constexpr float kSuperBurstGrowth = 3.8f;
inline constexpr float kSuperDashLengthFraction = 0.16f;
inline constexpr int kSuperDashR = 232;
inline constexpr int kSuperDashG = 170;
inline constexpr int kSuperDashB = 214;
// Round 2 keeps the form, damage, and clear run from round 1. Retry does not.
inline constexpr bool kCarryFormAcrossRounds = true;

// Rabbit → super only. Other rungs keep the drop burst and the form-change
// cue. The model swap is instant. This timer is the radial-dash burst.
struct SuperTransform {
    std::int64_t start_ms = 0;
    std::int64_t until_ms = 0;
    int sfx_id = kSuperTransformSfxId;
    int clip_id = kSuperTransformClipId;

    // True from the triggering hit until `until_ms`, exclusive.
    [[nodiscard]] bool active_at(std::int64_t now_ms) const {
        return until_ms > 0 && now_ms >= start_ms && now_ms < until_ms;
    }
};

struct PlayState {
    int event_index = 0;
    // Coupon score is capped. The uncapped earned total supplies the clear bonus.
    int score = 0;
    int earned_score = 0;
    int coupon_prior = 0;
    int earned_prior = 0;
    // Independent of the evolution ring, which resets on promotion.
    int score_streak = 0;
    // Distinct non-required actions performed before the next hit window.
    std::uint8_t freestyle = 0;
    Form form = Form::Rabbit;
    // Damage strokes on the current form. A clear sets this back to 0.
    int damage = 0;
    int clear_run = 0;
    int perfects = 0;
    int goods = 0;
    int misses = 0;
    bool paused = false;
    bool finished = false;
    Judgment last = Judgment::None;
    std::int64_t judged_ms = -100000;
    // Scribble burst is drawn while the clock is below this. 0 means none.
    // Form drops use this. The rabbit → super rung does not.
    std::int64_t burst_until_ms = 0;
    // Rabbit → super presentation. Idle when `until_ms` is 0.
    SuperTransform super_transform{};
    // Buttons gathered for the current obstacle. Combos may arrive across calls.
    std::uint8_t gathered = 0;
    // Set when this step resolves a miss. The next action press is the
    // post-crash press, then the flag clears unless that press is another miss.
    bool press_after_crash = false;
    // Misses since the last form change. Segmented ribbon jitter reads this.
    // A clear does not change it. A drop or a promotion sets it back to 0.
    int hits_since_form = 0;
    // Course time of the last miss that stayed on this form. Negative means
    // the jitter amplitude is the rest value.
    std::int64_t last_hit_ms = -1;
};

// Presses this step did not treat as a clear. Score, form, and judgment stay
// on PlayState. `whiff` is a press with no obstacle window open. `wrong` is a
// button that does not belong to the obstacle in range.
struct PlayAdvanceResult {
    // Press while no obstacle window was open, and this step did not miss one.
    bool whiff = false;
    // First press after a miss. False on the press that caused that miss.
    bool post_crash_press = false;
    // Wrong button while an obstacle window was open. A whiff is not a wrong press.
    bool wrong = false;
};

// Scores from the two courses of a pair. `paired` is set once round 1 has a total.
struct RoundScores {
    int earlier = 0;
    int latest = 0;
    bool paired = false;
};

// The number shown as the pair total. A single course is just `latest`.
[[nodiscard]] inline int pair_total(RoundScores scores) {
    return scores.paired ? scores.earlier + scores.latest : scores.latest;
}

// Perfect, good, and miss counts for one course.
struct RoundJudgment {
    int perfects = 0;
    int goods = 0;
    int misses = 0;
};

// A pair shows the sum. A single course shows `latest`.
[[nodiscard]] inline RoundJudgment
combined_judgment(RoundJudgment earlier, RoundJudgment latest, bool paired) {
    if (!paired) {
        return latest;
    }
    RoundJudgment sum;
    sum.perfects = earlier.perfects + latest.perfects;
    sum.goods = earlier.goods + latest.goods;
    sum.misses = earlier.misses + latest.misses;
    return sum;
}

// Round-start, then TITLE VAG 17. The mixer waits 1 s after the round-start
// voice ends before playing the sting. The music clock is not consulted, and
// the prelude stays kCourseStartDelayMs on every course, retry, and custom chart.
inline constexpr int kMusicStartSfxDelayMs = 1000;

[[nodiscard]] inline std::span<const SfxId> course_start_sequence() {
    static constexpr SfxId kIds[] = {SfxId::RoundStart, SfxId::MusicStart};
    return kIds;
}

// Failure plays game over when the course ends.
[[nodiscard]] inline std::optional<SfxId> course_failure_cue(Form form) {
    if (form == Form::Out) {
        return SfxId::GameOver;
    }
    return std::nullopt;
}

// Level complete plays on the results screen after the final course only.
[[nodiscard]] inline std::optional<SfxId> course_results_cue(Form form, bool final_course) {
    if (form == Form::Out || !final_course) {
        return std::nullopt;
    }
    return SfxId::LevelComplete;
}

// Copies form and damage when `carry` is set. The form-up clear run resets
// for the new round. Score, judgment counts, and the obstacle cursor stay as
// `next` already has them. Out is not carried.
inline void carry_form_into(PlayState& next, const PlayState& previous, bool carry) {
    if (!carry || previous.form == Form::Out) {
        return;
    }
    next.form = previous.form;
    next.damage = previous.damage;
    next.score_streak = previous.score_streak;
    // The session supplies prior totals after adding the completed stage bonus.
}

// One obstacle resolved by play_advance. `form_changed` is set when that hit
// raised or dropped the form. `super_transform` is set only for the rabbit
// → super rung. Timing and scoring are unchanged.
struct PlayHit {
    std::uint8_t obstacle = 0;
    Judgment judgment = Judgment::None;
    bool form_changed = false;
    bool super_transform = false;
};

// One point on the streak ring, in logical pixels.
struct StreakDot {
    float x = 0.f;
    float y = 0.f;
};

// How many streak dots to draw. Super and out stay empty. The count never
// passes the 17 slots, so an 18th dot is not drawn.
[[nodiscard]] inline int streak_ring_dots(Form form, int clear_run) {
    if (form == Form::Super || form == Form::Out || clear_run <= 0) {
        return 0;
    }
    return clear_run < kStreakSlotCount ? clear_run : kStreakSlotCount;
}

// Slot `index` on the clear-streak ellipse. 0 is 12 o'clock. Each next slot is
// `kStreakStepDeg` counter-clockwise on screen. `screen_h` is H, the 4:3
// game-area height. A new dot appears in that slot.
[[nodiscard]] StreakDot streak_ring_dot(float figure_x, float ribbon_y, float screen_h, int index);

// One radial dash of the rabbit → super burst, centered on radius r along
// the slot, so at u = 0 it straddles the ring dot it replaces.
// `u` is t / 0.28 s.
struct SuperDash {
    float x0 = 0.f;
    float y0 = 0.f;
    float x1 = 0.f;
    float y1 = 0.f;
    float radius = 0.f;
    float length = 0.f;
};

[[nodiscard]] float super_burst_u(std::int64_t start_ms, std::int64_t now_ms);

[[nodiscard]] SuperDash
super_burst_dash(float center_x, float center_y, float screen_h, int slot, float u);

// Good and perfect intervals on the audio clock. Perfect is 2 * kPerfectWindowMs
// wide and ends when the relevant outline's front (left) edge reaches Rabbit,
// plus kJudgmentLateBiasMs. Early good keeps the margin kGoodWindowMs -
// kPerfectWindowMs. Good then continues kGoodIntoObstacleMs past that end, into
// the obstacle. The base loop and loop-based pairs use the standalone loop's
// leading-edge offset, so pair-only spikes do not pull the success zone away
// from the loop. Other obstacles use their own leading edge. With no scroll
// speed the offset is skipped and perfect still ends at the hit plus the late
// bias.
struct ObstacleWindow {
    std::int64_t good_open = 0;
    std::int64_t good_close = 0;
    std::int64_t perfect_open = 0;
    std::int64_t perfect_close = 0;
};

// Perfect is kPerfectWindowMs * 2 wide and closes kJudgmentLateBiasMs after the
// outline's front edge reaches the figure, plus kLoopPerfectZoneShiftMs with a
// loop. Loop+pit and loop+wave take the plain loop's zone at their own cross.
// Good adds a margin before perfect and kGoodIntoObstacleMs after it.
[[nodiscard]] ObstacleWindow obstacle_window(const CourseEvent& event);

// Press time after calibration. `offset_ms` is subtracted, so 0 leaves the
// press where it was. A positive offset (taps after the click) judges earlier.
[[nodiscard]] inline std::int64_t calibrated_press_ms(std::int64_t press_ms, int offset_ms) {
    return press_ms - static_cast<std::int64_t>(offset_ms);
}

// Steps the course from `prev_ms` to `now_ms` on the audio clock.
// `edges` is the set of actions newly pressed at `now_ms`. A press is only
// collected inside the current obstacle's window, and it is not reused for the next one.
// `timing_offset_ms` shifts that press through `calibrated_press_ms` before the
// window test. Timeout misses stay on the audio clock. Zero matches a call
// that does not pass an offset.
// While paused, or after the course has finished, this is a no-op.
// `hits`, when set, receives each obstacle resolved by this call.
// The return value names an empty press, a wrong button, and the first press
// after a miss. None of those flags change the judgment. Callers that only
// need the judgment may ignore them.
PlayAdvanceResult play_advance(PlayState& state,
                               const CourseTimeline& course,
                               std::int64_t prev_ms,
                               std::int64_t now_ms,
                               std::uint8_t edges,
                               std::vector<PlayHit>* hits = nullptr,
                               int timing_offset_ms = 0);

[[nodiscard]] std::string_view form_name(Form form);

[[nodiscard]] std::string_view judgment_name(Judgment judgment);

[[nodiscard]] inline int form_miss_limit(Form form) {
    switch (form) {
    case Form::Super:
        return kSuperMisses;
    case Form::Rabbit:
        return kRabbitMisses;
    case Form::Frog:
        return kFrogMisses;
    case Form::Worm:
        return kWormMisses;
    case Form::Out:
        return 0;
    }
    return 0;
}

[[nodiscard]] inline int form_score_multiplier(Form form) {
    switch (form) {
    case Form::Super:
        return kSuperScoreMultiplier;
    case Form::Rabbit:
        return kRabbitScoreMultiplier;
    case Form::Frog:
        return kFrogScoreMultiplier;
    case Form::Worm:
        return kWormScoreMultiplier;
    case Form::Out:
        return kWormScoreMultiplier;
    }
    return 1;
}

// Song-progress fill for the bottom arc, in [0, 1]. A non-positive duration is full.
[[nodiscard]] inline float course_progress(std::int64_t now_ms, std::int32_t duration_ms) {
    if (duration_ms <= 0) {
        return 1.f;
    }
    if (now_ms <= 0) {
        return 0.f;
    }
    if (now_ms >= duration_ms) {
        return 1.f;
    }
    return static_cast<float>(now_ms) / static_cast<float>(duration_ms);
}

} // namespace oscilline
