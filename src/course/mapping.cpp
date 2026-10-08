// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Hit times, the distribution RNG, and approach so obstacles do not pass.

#include "oscilline/course/mapping.hpp"

#include "oscilline/course/attack.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/obstacle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace oscilline {
namespace {

constexpr std::int32_t kPatternBreak = 0;
constexpr std::int32_t kPatternFixed = 1;
constexpr std::int32_t kPatternDistribution = 2;
constexpr std::uint32_t kLcgMul = 1664525u;
constexpr std::uint32_t kLcgAdd = 1013904223u;
constexpr std::int32_t kIntMax = 0x7fffffff;

// Numerical Recipes LCG. The low bits alternate, so a remainder would make a
// 50:50 section strict B/W (or L/W) alternation. The roll uses the high half.
std::uint32_t lcg_step(std::uint32_t state) {
    return state * kLcgMul + kLcgAdd;
}

std::uint32_t high_roll(std::uint32_t state, std::uint32_t max_prob) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * max_prob) >> 32);
}

const FslControlSegment* segment_at(const FslControlTrack& track, std::int32_t time) {
    const FslControlSegment* chosen = nullptr;
    for (const FslControlSegment& segment : track.segments) {
        if (segment.start_time <= time &&
            (chosen == nullptr || segment.start_time >= chosen->start_time)) {
            chosen = &segment;
        }
    }
    return chosen;
}

std::int64_t control_units(const FslControlTrack& track, std::int32_t time, bool speed) {
    const FslControlSegment* chosen = segment_at(track, time);
    if (chosen == nullptr) {
        return speed ? kMinApproachSpeed : 0;
    }
    const std::int64_t base = speed ? chosen->base_speed : chosen->base_shadow_period;
    const std::int64_t delta = speed ? chosen->delta_speed : chosen->delta_shadow_period;
    std::int64_t dt = static_cast<std::int64_t>(time) - chosen->start_time;
    if (dt < 0) {
        dt = 0;
    }
    // FSL control value: base + (t - start) * delta / 4096, toward zero.
    return base + (dt * delta) / 4096;
}

std::int32_t units_to_ms(std::int64_t units, double units_per_second) {
    if (units <= 0) {
        return 0;
    }
    const double ms = static_cast<double>(units) * 1000.0 / units_per_second;
    if (ms >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    return static_cast<std::int32_t>(std::llround(ms));
}

std::int32_t ms_to_units(std::int32_t ms, double units_per_second) {
    if (ms <= 0) {
        return 0;
    }
    const double units = static_cast<double>(ms) * units_per_second / 1000.0;
    if (units >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    return static_cast<std::int32_t>(std::llround(units));
}

double units_to_ms_f(double units, double units_per_second) {
    return units * 1000.0 / units_per_second;
}

double clamp_beat_units(double speed, double units_per_second) {
    if (speed < static_cast<double>(kMinApproachSpeed)) {
        speed = static_cast<double>(kMinApproachSpeed);
    }
    const double lo = static_cast<double>(kMinBeatMs) * units_per_second / 1000.0;
    const double hi = static_cast<double>(kMaxBeatMs) * units_per_second / 1000.0;
    return std::clamp(speed, lo, hi);
}

// The speed value read as a beat period, in units, clamped to kMinBeatMs..kMaxBeatMs.
double beat_units(const FslControlTrack& track, std::int32_t time, double units_per_second) {
    return clamp_beat_units(static_cast<double>(control_units(track, time, true)),
                            units_per_second);
}

std::int32_t approach_ms(double beat, double units_per_second);

// One scroll speed per control segment, taken from that segment's base speed so a
// ramp inside the segment does not change the picture's speed. A later obstacle
// that would enter before the previous one is sped up until it does not.
void assign_scroll_approaches(std::vector<CourseEvent>& events,
                              const FslControlTrack& control,
                              double units_per_second) {
    for (CourseEvent& event : events) {
        const std::int32_t units = ms_to_units(event.hit_ms, units_per_second);
        const FslControlSegment* segment = segment_at(control, units);
        const double speed = segment == nullptr ? static_cast<double>(kMinApproachSpeed)
                                                : static_cast<double>(segment->base_speed);
        const double beat = clamp_beat_units(speed, units_per_second);
        event.scroll_approach_ms = approach_ms(beat, units_per_second);
        event.beat_ms =
            units_to_ms(static_cast<std::int64_t>(std::llround(beat)), units_per_second);
    }
    for (std::size_t i = 1; i < events.size(); ++i) {
        const CourseEvent& prev = events[i - 1];
        CourseEvent& event = events[i];
        const std::int64_t entry_prev =
            static_cast<std::int64_t>(prev.hit_ms) - prev.scroll_approach_ms;
        const std::int64_t max_approach = static_cast<std::int64_t>(event.hit_ms) - entry_prev;
        if (max_approach < 1) {
            event.scroll_approach_ms = 1;
        } else if (event.scroll_approach_ms > max_approach) {
            event.scroll_approach_ms =
                static_cast<std::int32_t>(std::min<std::int64_t>(max_approach, kIntMax));
        }
    }
}

// Shadow in units. A ramp never takes it below the segment's base shadow.
double shadow_units(const FslControlTrack& track, std::int32_t time) {
    const FslControlSegment* chosen = segment_at(track, time);
    const std::int64_t base = chosen == nullptr ? 0 : chosen->base_shadow_period;
    std::int64_t shadow = control_units(track, time, false);
    if (shadow < base) {
        shadow = base;
    }
    return shadow < 0 ? 0.0 : static_cast<double>(shadow);
}

// Whole beats the shadow asks for, at least one.
double shadow_beats(double shadow, double beat) {
    const double beats = std::ceil(shadow / beat - kShadowBeatTolerance);
    return beats < 1.0 ? 1.0 : beats;
}

std::int32_t approach_ms(double beat, double units_per_second) {
    const double ms = units_to_ms_f(beat * kApproachBeats, units_per_second);
    return static_cast<std::int32_t>(std::llround(
        std::clamp(ms, static_cast<double>(kMinApproachMs), static_cast<double>(kMaxApproachMs))));
}

struct Track {
    const FslControlTrack* control = nullptr;
    const FslPatternTrack* patterns = nullptr;
};

Result<Track> course_track(const FslFile& file, int track_index, const CourseMapOptions& options) {
    if (!(options.units_per_second > 0.0) || !std::isfinite(options.units_per_second)) {
        return Result<Track>::failure(
            "course time base must be a positive number of units per second");
    }
    if (track_index < 0 || static_cast<std::uint32_t>(track_index) >= file.track_index.size()) {
        return Result<Track>::failure("course index is outside the track table");
    }
    const FslTrackIndex& index = file.track_index[static_cast<std::size_t>(track_index)];
    if (index.control_segment_index >= file.control_tracks.size()) {
        return Result<Track>::failure("course control track is missing");
    }
    if (index.pattern_segment_index >= file.pattern_tracks.size()) {
        return Result<Track>::failure("course pattern track is missing");
    }
    Track track;
    track.control = &file.control_tracks[static_cast<std::size_t>(index.control_segment_index)];
    track.patterns = &file.pattern_tracks[static_cast<std::size_t>(index.pattern_segment_index)];
    return Result<Track>::success(track);
}

// Speed or shadow for attack picking, in units. A ramp heads for the next
// segment's base and stops there; the last segment holds its base. Course 1's
// last segment carries a ramp that would otherwise run away.
std::int64_t attack_control_units(const FslControlTrack& track, std::int32_t time, bool speed) {
    const FslControlSegment* chosen = segment_at(track, time);
    if (chosen == nullptr) {
        return 0;
    }
    const FslControlSegment* next = nullptr;
    for (const FslControlSegment& segment : track.segments) {
        if (segment.start_time > chosen->start_time &&
            (next == nullptr || segment.start_time < next->start_time)) {
            next = &segment;
        }
    }
    const std::int64_t base = speed ? chosen->base_speed : chosen->base_shadow_period;
    if (next == nullptr) {
        return std::max<std::int64_t>(base, 0);
    }
    const std::int64_t delta = speed ? chosen->delta_speed : chosen->delta_shadow_period;
    const std::int64_t next_base = speed ? next->base_speed : next->base_shadow_period;
    const std::int64_t dt =
        std::max<std::int64_t>(static_cast<std::int64_t>(time) - chosen->start_time, 0);
    const std::int64_t value = base + (dt * delta) / 4096;
    const std::int64_t lo = std::min(base, next_base);
    const std::int64_t hi = std::max(base, next_base);
    return std::max<std::int64_t>(std::clamp(value, lo, hi), 0);
}

std::int32_t to_units(double units) {
    if (!(units > 0.0)) {
        return 0;
    }
    if (units >= static_cast<double>(kIntMax)) {
        return kIntMax;
    }
    return static_cast<std::int32_t>(units);
}

bool in_break(const FslPatternTrack& patterns, double units) {
    const auto time = to_units(units);
    const FslPatternSegment* chosen = nullptr;
    for (const FslPatternSegment& segment : patterns.segments) {
        if (segment.start_time <= time &&
            (chosen == nullptr || segment.start_time >= chosen->start_time)) {
            chosen = &segment;
        }
    }
    return chosen != nullptr && chosen->pattern_type == kPatternBreak;
}

// Attack selection, all times in units (constants and their provenance in
// course/attack.hpp). Select windows are laid back to back from 0; each is the
// shadow value long, read at the pending attack (or, with nothing pending, at
// the closed window's best evaluation, or its start), and offers its largest
// evaluation as a potential attack. An evaluation stamped exactly on a window
// end belongs to the next window. At each window end:
// - a potential before the commit time of the pending attack replaces it when
//   its emphasis is more than kAttackReplaceRatio times larger;
// - otherwise a pending attack whose commit time (attack + kAttackCommitSpeeds
//   speed periods, speed read at the attack) has passed is committed;
// - a potential inside a break, or with no emphasis, is ignored;
// - with nothing pending, a potential at least kAttackCommitSpeeds speed
//   periods after the last committed attack becomes the pending attack.
std::vector<double> pick_attacks(std::span<const float> emphasis,
                                 const FslControlTrack& control,
                                 const FslPatternTrack& patterns,
                                 double units_per_second,
                                 std::int32_t audio_end_ms) {
    struct Candidate {
        double time = 0.0;
        float emphasis = 0.0f;
    };
    // kAttackCommitSpeeds speed periods, with the speed read at `time`.
    const auto commit_gap = [&](double time) {
        return static_cast<double>(attack_control_units(control, to_units(time), true)) *
               kAttackCommitSpeeds;
    };
    const auto window_length = [&](double time) {
        return std::max<double>(
            static_cast<double>(attack_control_units(control, to_units(time), false)), 1.0);
    };
    std::vector<double> attacks;
    bool committed = false;
    double last = 0.0;
    bool pending = false;
    Candidate pend;
    const auto end_window = [&](double tick, bool have, const Candidate& potential) {
        if (pending) {
            const double commit_at = pend.time + commit_gap(pend.time);
            const bool replaces = have && potential.time < commit_at &&
                                  potential.emphasis > kAttackReplaceRatio * pend.emphasis &&
                                  !in_break(patterns, potential.time);
            if (!replaces && tick >= commit_at) {
                attacks.push_back(pend.time);
                last = pend.time;
                committed = true;
                pending = false;
            }
        }
        if (!have || !(potential.emphasis > 0.0f) || in_break(patterns, potential.time)) {
            return;
        }
        if (!pending) {
            if (!committed || potential.time >= last + commit_gap(last)) {
                pend = potential;
                pending = true;
            }
        } else if (potential.emphasis > kAttackReplaceRatio * pend.emphasis) {
            pend = potential;
        }
    };
    const double units_per_ms = units_per_second / 1000.0;
    // 0 keeps the old "emphasis ran out" flush. A real track stops at its end,
    // so a pending attack whose commit falls after the music is not placed.
    const double limit = audio_end_ms > 0
                             ? static_cast<double>(ms_to_units(audio_end_ms, units_per_second))
                             : std::numeric_limits<double>::infinity();
    double window_start = 0.0;
    double window_end = window_length(0.0);
    bool have = false;
    Candidate best;
    for (std::size_t k = 0; k < emphasis.size(); ++k) {
        // Exact on the default 22050 units per second: stamps are whole units.
        const double time = static_cast<double>(attack_stamp_frame(k)) * units_per_second / 44100.0;
        if (time >= limit) {
            break;
        }
        while (time >= window_end && window_end < limit) {
            end_window(window_end, have, best);
            const double at = pending ? pend.time : (have ? best.time : window_start);
            have = false;
            window_start = window_end;
            window_end = window_start + window_length(at);
        }
        if (!have || emphasis[k] > best.emphasis) {
            best = {time, emphasis[k]};
            have = true;
        }
    }
    const bool best_in = have && best.time < limit;
    end_window(limit, best_in, best);
    if (pending) {
        const double commit_at = pend.time + commit_gap(pend.time);
        if (pend.time < limit && commit_at <= limit) {
            attacks.push_back(pend.time);
        }
    }
    for (double& time : attacks) {
        time /= units_per_ms;
    }
    return attacks;
}

} // namespace

Result<std::vector<double>> course_attack_ms(const FslFile& file,
                                             int track_index,
                                             std::span<const float> emphasis,
                                             CourseMapOptions options,
                                             std::int32_t audio_end_ms) {
    auto track = course_track(file, track_index, options);
    if (!track) {
        return Result<std::vector<double>>::failure(track.error());
    }
    return Result<std::vector<double>>::success(pick_attacks(emphasis,
                                                             *track.value().control,
                                                             *track.value().patterns,
                                                             options.units_per_second,
                                                             audio_end_ms));
}

int distribution_index(std::span<const std::int32_t> obstacle_prob, std::int32_t roll) {
    const std::size_t count = std::min<std::size_t>(obstacle_prob.size(), kObstacleKindCount);
    for (std::size_t i = 0; i < count; ++i) {
        if (roll < obstacle_prob[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Result<double> course_beat_period_ms(const FslFile& file,
                                     int track_index,
                                     std::int32_t time_ms,
                                     CourseMapOptions options) {
    auto track = course_track(file, track_index, options);
    if (!track) {
        return Result<double>::failure(track.error());
    }
    const std::int32_t time = ms_to_units(time_ms, options.units_per_second);
    return Result<double>::success(
        units_to_ms_f(beat_units(*track.value().control, time, options.units_per_second),
                      options.units_per_second));
}

bool obstacle_after_audio(std::int32_t hit_ms, std::int32_t travel_ms, std::int32_t audio_end_ms) {
    if (audio_end_ms <= 0) {
        return false;
    }
    if (hit_ms > audio_end_ms) {
        return true;
    }
    const std::int64_t travel = travel_ms > 0 ? travel_ms : 0;
    return static_cast<std::int64_t>(hit_ms) - travel >= audio_end_ms;
}

void drop_obstacles_after_audio(std::vector<CourseEvent>& events, std::int32_t audio_end_ms) {
    if (audio_end_ms <= 0) {
        return;
    }
    std::erase_if(events, [&](const CourseEvent& event) {
        const std::int32_t travel = stage_scroll_approach_ms(event);
        return obstacle_after_audio(event.hit_ms, travel, audio_end_ms);
    });
}

Result<CourseTimeline> build_course(const FslFile& file,
                                    int track_index,
                                    std::int32_t audio_duration_ms,
                                    CourseMapOptions options) {
    auto track = course_track(file, track_index, options);
    if (!track) {
        return Result<CourseTimeline>::failure(track.error());
    }
    const FslControlTrack& control = *track.value().control;
    const FslPatternTrack& patterns = *track.value().patterns;
    const double units_per_second = options.units_per_second;

    CourseTimeline timeline;
    timeline.track_index = track_index;
    timeline.cdda_track = track_index + 2;
    bool clamped = false;

    std::vector<std::size_t> order(patterns.segments.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return patterns.segments[a].start_time < patterns.segments[b].start_time;
    });

    std::vector<std::int32_t> beats;
    beats.reserve(options.beat_ms.size());
    for (const std::int32_t ms : options.beat_ms) {
        if (ms >= 0 && (beats.empty() || ms_to_units(ms, units_per_second) > beats.back())) {
            beats.push_back(ms_to_units(ms, units_per_second));
        }
    }
    // Attack mode: hits follow attacks picked from the emphasis. Slots are the
    // attack times and each obstacle collides at its audible attack.
    const bool use_attacks = !options.emphasis.empty();
    if (use_attacks) {
        beats.clear();
        for (const double ms : pick_attacks(
                 options.emphasis, control, patterns, units_per_second, audio_duration_ms)) {
            const std::int32_t unit =
                ms_to_units(static_cast<std::int32_t>(std::llround(ms)), units_per_second);
            if (beats.empty() || unit > beats.back()) {
                beats.push_back(unit);
            }
        }
    }
    const bool use_beats = use_attacks || !beats.empty();
    // Half a unit of slack so a gap of exactly kMinGapMs survives rounding.
    const double floor_units = static_cast<double>(kMinGapMs) * units_per_second / 1000.0 - 0.5;

    // Last accepted slot, in units. Spacing carries across pattern segments.
    double last = -1.0;
    const auto accept = [&](std::int32_t slot) {
        if (use_attacks || last < 0.0) {
            return true;
        }
        const auto at = static_cast<std::int32_t>(last);
        const double beat = beat_units(control, at, units_per_second);
        const double need = shadow_beats(shadow_units(control, at), beat);
        const double gap = static_cast<double>(slot) - last;
        if (gap < (need - kSlotAcceptSlackBeats) * beat) {
            return false;
        }
        if (gap < floor_units) {
            clamped = true;
            return false;
        }
        return true;
    };
    // Parallel to timeline.events. Null for a fixed obstacle. A distribution
    // pointer is resolved after cuts, so a removed event takes no random step.
    std::vector<const FslDistributionPattern*> type_source;
    const auto push_event =
        [&](std::int32_t slot, std::uint32_t obstacle, const FslDistributionPattern* source) {
            if (!obstacle_known(obstacle) ||
                static_cast<int>(timeline.events.size()) >= kMaxCourseEvents) {
                return;
            }
            CourseEvent event;
            event.obstacle = static_cast<std::uint8_t>(obstacle);
            event.hit_ms = units_to_ms(slot, units_per_second);
            event.approach_ms =
                approach_ms(beat_units(control, slot, units_per_second), units_per_second);
            timeline.events.push_back(event);
            type_source.push_back(source);
        };

    for (std::size_t n = 0; n < order.size(); ++n) {
        if (static_cast<int>(timeline.events.size()) >= kMaxCourseEvents) {
            break;
        }
        const FslPatternSegment& segment = patterns.segments[order[n]];
        if (segment.pattern_type != kPatternFixed && segment.pattern_type != kPatternDistribution) {
            continue;
        }
        if (segment.index < 0) {
            continue;
        }
        const std::int32_t tail = ms_to_units(kTailMsWithoutAudio, units_per_second);
        std::int32_t end =
            segment.start_time > kIntMax - tail ? kIntMax : segment.start_time + tail;
        if (n + 1 < order.size()) {
            end = patterns.segments[order[n + 1]].start_time;
        } else if (audio_duration_ms > 0) {
            end = ms_to_units(audio_duration_ms, units_per_second);
        }
        if (end <= segment.start_time) {
            continue;
        }

        const FslFixedPattern* fixed = nullptr;
        const FslDistributionPattern* distribution = nullptr;
        if (segment.pattern_type == kPatternFixed) {
            if (static_cast<std::uint32_t>(segment.index) >= file.fixed_patterns.size()) {
                return Result<CourseTimeline>::failure("fixed pattern index is outside the table");
            }
            fixed = &file.fixed_patterns[static_cast<std::size_t>(segment.index)];
            if (fixed->obstacles.empty()) {
                continue;
            }
        } else {
            if (static_cast<std::uint32_t>(segment.index) >= file.distribution_patterns.size()) {
                return Result<CourseTimeline>::failure(
                    "distribution pattern index is outside the table");
            }
            distribution = &file.distribution_patterns[static_cast<std::size_t>(segment.index)];
            if (distribution->max_prob <= 0) {
                continue;
            }
        }

        std::size_t cursor = 0;
        const auto take = [&](std::int32_t slot) {
            if (!accept(slot)) {
                return;
            }
            last = static_cast<double>(slot);
            if (fixed != nullptr) {
                push_event(slot, fixed->obstacles[cursor % fixed->obstacles.size()], nullptr);
                ++cursor;
                return;
            }
            // Placeholder type. The real id is chosen after lead-in cuts.
            push_event(slot, 0, distribution);
        };

        if (use_beats) {
            auto it = std::lower_bound(beats.begin(), beats.end(), segment.start_time);
            for (; it != beats.end() && *it < end; ++it) {
                if (static_cast<int>(timeline.events.size()) >= kMaxCourseEvents) {
                    break;
                }
                take(*it);
            }
            continue;
        }
        // No beats: a grid of the speed period from the segment start.
        double slot = static_cast<double>(segment.start_time);
        while (slot < static_cast<double>(end) &&
               static_cast<int>(timeline.events.size()) < kMaxCourseEvents) {
            take(static_cast<std::int32_t>(std::llround(slot)));
            slot += beat_units(control, static_cast<std::int32_t>(slot), units_per_second);
        }
    }

    // Spawn order follows hit order, so no obstacle overtakes an earlier one.
    // First an earlier obstacle may start sooner (a longer approach, up to the
    // maximum). Any obstacle that would still start before its predecessor then
    // gets a shorter approach instead.
    auto& events = timeline.events;
    for (std::size_t i = events.size(); i-- > 1;) {
        const std::int64_t next_spawn =
            static_cast<std::int64_t>(events[i].hit_ms) - events[i].approach_ms;
        const std::int64_t spawn =
            static_cast<std::int64_t>(events[i - 1].hit_ms) - events[i - 1].approach_ms;
        if (spawn > next_spawn) {
            const std::int64_t wanted =
                static_cast<std::int64_t>(events[i - 1].hit_ms) - next_spawn;
            events[i - 1].approach_ms =
                static_cast<std::int32_t>(std::min<std::int64_t>(wanted, kMaxApproachMs));
        }
    }
    for (std::size_t i = 1; i < events.size(); ++i) {
        const std::int64_t prev_spawn =
            static_cast<std::int64_t>(events[i - 1].hit_ms) - events[i - 1].approach_ms;
        const std::int64_t spawn =
            static_cast<std::int64_t>(events[i].hit_ms) - events[i].approach_ms;
        if (spawn < prev_spawn) {
            events[i].approach_ms = static_cast<std::int32_t>(events[i].hit_ms - prev_spawn);
        }
    }

    {
        std::vector<CourseEvent> kept;
        std::vector<const FslDistributionPattern*> kept_source;
        kept.reserve(events.size());
        kept_source.reserve(events.size());
        for (std::size_t i = 0; i < events.size(); ++i) {
            const CourseEvent& event = events[i];
            if (obstacle_after_audio(event.hit_ms, event.approach_ms, audio_duration_ms)) {
                continue;
            }
            // The lead-in cut is for beat and grid slots only. The original
            // keeps its early attacks, and each takes a random step.
            const std::int64_t ready = static_cast<std::int64_t>(event.approach_ms) + kLeadInMs;
            if (!use_attacks && event.hit_ms < ready) {
                continue;
            }
            kept.push_back(event);
            kept_source.push_back(type_source[i]);
        }
        events.swap(kept);
        type_source.swap(kept_source);
    }

    // Observed on all six courses: a random section starts the state at
    // random_seed * course_number (1-based), not 2 * tier + 1. Course 1 with
    // seed 11 starts at 11, course 2 with seed 4 at 8, course 3 with seed 311
    // at 933, course 4 with seed 4 at 16, and course 6 with seeds 603 and 602
    // at 3618 and 3612. A section with a different record reseeds from it
    // (course 1's second record, seed 8, starts at 8). Course 3 keeps one state
    // across its three sections that share a record. Each obstacle takes one
    // step in hit order, including the attacks in the first seconds: on courses
    // 1 and 3 the attack at about 0.5 s takes the first step. Course 5's fixed
    // sections have no seeded state, so a fixed obstacle takes no step.
    const std::uint32_t course_number = static_cast<std::uint32_t>(track_index) + 1u;
    const FslDistributionPattern* seeded = nullptr;
    std::uint32_t rng = 0;
    std::vector<CourseEvent> spawned;
    spawned.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const FslDistributionPattern* distribution = type_source[i];
        if (distribution == nullptr) {
            spawned.push_back(events[i]);
            continue;
        }
        if (distribution != seeded) {
            seeded = distribution;
            rng = static_cast<std::uint32_t>(distribution->random_seed) * course_number;
        }
        rng = lcg_step(rng);
        const auto max_prob = static_cast<std::uint32_t>(distribution->max_prob);
        const std::uint32_t roll = high_roll(rng, max_prob);
        const int picked =
            distribution_index(distribution->obstacle_prob, static_cast<std::int32_t>(roll));
        if (picked < 0) {
            continue;
        }
        events[i].obstacle = static_cast<std::uint8_t>(picked);
        spawned.push_back(events[i]);
    }
    events.swap(spawned);
    assign_scroll_approaches(events, control, units_per_second);
    // Scroll can pull a spawn onto the far side of the track end. Those hits
    // already took no step when the approach itself was past the audio.
    drop_obstacles_after_audio(events, audio_duration_ms);

    // The later of the audio end and one second after the last hit. Hits past
    // the audio were already dropped, so nothing new is spawned after it ends.
    std::int64_t duration = 0;
    if (!events.empty()) {
        duration = static_cast<std::int64_t>(events.back().hit_ms) + kCourseEndTailMs;
    }
    if (audio_duration_ms > duration) {
        duration = audio_duration_ms;
    }
    duration = std::clamp<std::int64_t>(duration, 0, kIntMax);
    timeline.duration_ms = static_cast<std::int32_t>(duration);
    timeline.audio_end_ms = audio_duration_ms > 0 ? audio_duration_ms : 0;
    timeline.camera_sections =
        camera_sections_from_patterns(patterns.segments, audio_duration_ms, units_per_second);
    const CoursePair gold = courses_for(Difficulty::Gold);
    timeline.gold_shift = track_index >= gold.first && track_index <= gold.second;
    if (clamped) {
        std::cerr << "oscilline: obstacle gap held at " << kMinGapMs << " ms\n";
    }
    return Result<CourseTimeline>::success(std::move(timeline));
}

} // namespace oscilline
