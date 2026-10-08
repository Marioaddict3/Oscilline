// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Onset chart, type sequence, phrase copy, and the music score file.

#include "oscilline/course/music.hpp"

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/beats.hpp"
#include "oscilline/course/play.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

constexpr float kStrongMad = 3.f;
constexpr float kWeakMad = 1.f;
constexpr float kStrongFloor = 0.12f;
constexpr float kWeakFloor = 0.035f;
constexpr int kAdaptFrames = 50;

int approach_beats_for(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Bronze:
        return kBronzeApproachBeats;
    case Difficulty::Silver:
        return kSilverApproachBeats;
    case Difficulty::Gold:
        return kGoldApproachBeats;
    }
    return kBronzeApproachBeats;
}

std::int32_t approach_for(int period_ms, Difficulty difficulty) {
    const int beats = period_ms * approach_beats_for(difficulty);
    return std::clamp(beats, kMinApproachMs, kMaxApproachMs);
}

// Experimental placement still has one onset curve. Scale it so bronze stays
// easier than silver and gold stays harder.
float difficulty_density_scale(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Bronze:
        return 1.f;
    case Difficulty::Silver:
        return 1.25f;
    case Difficulty::Gold:
        return 1.5f;
    }
    return 1.f;
}

std::uint8_t obstacle_for(int kind, bool pair) {
    switch (kind) {
    case 0:
        return pair ? 4 : 0;
    case 1:
        return pair ? 8 : 1;
    case 2:
        return pair ? 9 : 3;
    case 3:
        return pair ? 7 : 2;
    default:
        return 2;
    }
}

void keep_spawn_order(std::vector<CourseEvent>& events) {
    for (std::size_t i = 1; i < events.size(); ++i) {
        CourseEvent& previous = events[i - 1];
        CourseEvent& event = events[i];
        const std::int64_t previous_entry =
            static_cast<std::int64_t>(previous.hit_ms) - previous.scroll_approach_ms;
        const std::int64_t entry =
            static_cast<std::int64_t>(event.hit_ms) - event.scroll_approach_ms;
        if (entry < previous_entry) {
            const std::int64_t span = static_cast<std::int64_t>(event.hit_ms) - previous_entry;
            event.scroll_approach_ms = span < 1 ? 1 : static_cast<std::int32_t>(span);
        }
    }
}

float median_of(std::vector<float> values) {
    if (values.empty()) {
        return 0.f;
    }
    const std::size_t mid = values.size() / 2;
    std::nth_element(
        values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    return values[mid];
}

struct Prepared {
    std::vector<std::int16_t> pcm;
    int frames = 0;
    int rate = kCddaRate;
    int window = kOnsetWindowFrames;
    int hop = kOnsetHopFrames;
};

Prepared decimate_music(std::span<const std::int16_t> interleaved, int frames) {
    Prepared out;
    const int factor = kMusicDecimate;
    if (factor < 2 || frames / factor < kMusicWindowFrames) {
        const std::size_t samples = static_cast<std::size_t>(frames) * 2u;
        out.pcm.assign(interleaved.begin(),
                       interleaved.begin() + static_cast<std::ptrdiff_t>(samples));
        out.frames = frames;
        return out;
    }
    const int count = frames / factor;
    out.pcm.resize(static_cast<std::size_t>(count) * 2u);
    for (int i = 0; i < count; ++i) {
        int left = 0;
        int right = 0;
        const int base = i * factor;
        for (int k = 0; k < factor; ++k) {
            const std::size_t frame = static_cast<std::size_t>(base + k);
            left += interleaved[frame * 2u];
            right += interleaved[frame * 2u + 1u];
        }
        out.pcm[static_cast<std::size_t>(i) * 2u] = static_cast<std::int16_t>(left / factor);
        out.pcm[static_cast<std::size_t>(i) * 2u + 1u] = static_cast<std::int16_t>(right / factor);
    }
    out.frames = count;
    out.rate = kCddaRate / factor;
    out.window = kMusicWindowFrames;
    out.hop = kMusicHopFrames;
    return out;
}

double hop_ms(const OnsetAnalysis& analysis) {
    return static_cast<double>(analysis.hop_frames) * 1000.0 /
           static_cast<double>(analysis.sample_rate);
}

std::int32_t time_of(double index, const OnsetAnalysis& analysis) {
    const double ms = index * hop_ms(analysis);
    if (ms <= 0.0) {
        return 0;
    }
    return static_cast<std::int32_t>(std::llround(ms));
}

int frame_at(std::int32_t ms, const OnsetAnalysis& analysis) {
    const double step = hop_ms(analysis);
    if (!(step > 0.0) || analysis.envelope.empty()) {
        return 0;
    }
    const int index = static_cast<int>(std::llround(static_cast<double>(ms) / step));
    return std::clamp(index, 0, static_cast<int>(analysis.envelope.size()) - 1);
}

float pitch_of(const OnsetAnalysis& analysis, int frame) {
    if (frame < 0 || frame >= static_cast<int>(analysis.centroid_hz.size())) {
        return 0.f;
    }
    const float low = analysis.low_centroid_hz[static_cast<std::size_t>(frame)];
    if (low >= 30.f) {
        return low;
    }
    const float full = analysis.centroid_hz[static_cast<std::size_t>(frame)];
    return full >= 30.f ? full : 0.f;
}

// Pitch change through the body of the hit, so the silence before an attack
// does not look like a rise.
float rise_across(const OnsetAnalysis& analysis, int frame) {
    const int n = static_cast<int>(analysis.centroid_hz.size());
    if (n < 2) {
        return 0.f;
    }
    const int late = std::min(
        n - 1, frame + std::max(2, static_cast<int>(std::lround(22.0 / hop_ms(analysis)))));
    const float here = pitch_of(analysis, frame);
    const float then = pitch_of(analysis, late);
    if (here >= 30.f && then >= 30.f) {
        return then - here;
    }
    return 0.f;
}

float envelope_at(std::int32_t ms, const OnsetAnalysis& analysis) {
    if (analysis.envelope.empty()) {
        return 0.f;
    }
    return analysis.envelope[static_cast<std::size_t>(frame_at(ms, analysis))];
}

int quantise_bpm(int bpm, int min_bpm, int max_bpm) {
    const int quantum = std::max(1, kTempoBpmQuantum);
    const int clamped = std::clamp(bpm, min_bpm, max_bpm);
    const int down = clamped - (clamped % quantum);
    const int up = down + quantum;
    int snapped = down;
    if (up <= max_bpm &&
        (clamped - down > up - clamped ||
         (clamped - down == up - clamped &&
          std::abs(up - kTempoReferenceBpm) < std::abs(down - kTempoReferenceBpm)))) {
        snapped = up;
    }
    if (snapped < min_bpm) {
        snapped = up <= max_bpm ? up : clamped;
    }
    return std::clamp(snapped, min_bpm, max_bpm);
}

// Integer BPM from a band-limited envelope, snapped to kTempoBpmQuantum and
// nudged toward 120 when a neighbor scores within a few percent.
int estimate_period_ms(const std::vector<float>& envelope, int hop, int rate) {
    const int n = static_cast<int>(envelope.size());
    const int min_bpm = std::max(30, (60000 + kMaxBeatMs - 1) / kMaxBeatMs);
    const int max_bpm = std::min(240, 60000 / kMinBeatMs);
    if (n < 16 || hop < 1 || rate < 1000) {
        return kDefaultMusicBeatMs;
    }
    double mean = 0.0;
    for (const float value : envelope) {
        mean += value;
    }
    mean /= static_cast<double>(n);
    std::vector<double> smooth(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        double sum = 0.0;
        int count = 0;
        for (int k = -6; k <= 6; ++k) {
            const int j = i + k;
            if (j < 0 || j >= n) {
                continue;
            }
            sum += static_cast<double>(envelope[static_cast<std::size_t>(j)]) - mean;
            ++count;
        }
        smooth[static_cast<std::size_t>(i)] = count > 0 ? sum / static_cast<double>(count) : 0.0;
    }
    const double ms_per = static_cast<double>(hop) * 1000.0 / static_cast<double>(rate);
    std::vector<double> scores(static_cast<std::size_t>(max_bpm + 1), -1.0e300);
    double best = -1.0e300;
    int best_bpm = kTempoReferenceBpm;
    for (int bpm = min_bpm; bpm <= max_bpm; ++bpm) {
        const double lag = (60000.0 / static_cast<double>(bpm)) / ms_per;
        const int lag0 = static_cast<int>(std::floor(lag));
        const double frac = lag - static_cast<double>(lag0);
        if (lag0 < 1 || lag0 + 1 >= n / 2) {
            continue;
        }
        double score = 0.0;
        int count = 0;
        const auto accumulate = [&](int lag_frames, double weight) {
            if (lag_frames < 1 || lag_frames >= n / 2) {
                return;
            }
            for (int i = 0; i + lag_frames < n; i += 4) {
                score += weight * smooth[static_cast<std::size_t>(i)] *
                         smooth[static_cast<std::size_t>(i + lag_frames)];
                ++count;
            }
        };
        accumulate(lag0, 1.0 - frac);
        accumulate(lag0 + 1, frac);
        accumulate(lag0 * 2, 0.5);
        accumulate(lag0 * 3, 0.25);
        if (count > 0) {
            score /= static_cast<double>(count);
        }
        const double octave =
            std::log(static_cast<double>(bpm) / static_cast<double>(kTempoReferenceBpm)) /
            std::log(2.0);
        score *= std::exp(-0.5 * octave * octave);
        scores[static_cast<std::size_t>(bpm)] = score;
        if (score > best) {
            best = score;
            best_bpm = bpm;
        }
    }
    if (!(best > 0.0)) {
        return kDefaultMusicBeatMs;
    }
    int chosen = best_bpm;
    int chosen_dist = std::abs(best_bpm - kTempoReferenceBpm);
    const double gate = best * (1.0 - static_cast<double>(kTempoHysteresis));
    for (int bpm = min_bpm; bpm <= max_bpm; ++bpm) {
        const double score = scores[static_cast<std::size_t>(bpm)];
        if (score < gate) {
            continue;
        }
        const int dist = std::abs(bpm - kTempoReferenceBpm);
        if (dist < chosen_dist ||
            (dist == chosen_dist && score > scores[static_cast<std::size_t>(chosen)])) {
            chosen = bpm;
            chosen_dist = dist;
        }
    }
    const auto nearer = [&](int candidate) {
        if (candidate < min_bpm || candidate > max_bpm) {
            return;
        }
        if (scores[static_cast<std::size_t>(candidate)] < best * kTempoDoubleFit) {
            return;
        }
        if (std::abs(candidate - kTempoReferenceBpm) < std::abs(chosen - kTempoReferenceBpm)) {
            chosen = candidate;
        }
    };
    nearer(chosen * 2);
    if (chosen >= 2) {
        nearer(chosen / 2);
    }
    // A half-tempo estimate yields to the beat when both fit, so a 170 BPM
    // track charts at 170 and not 85. The cap keeps 120 from doubling to 240.
    const int faster = chosen * 2;
    if (faster <= std::min(max_bpm, kTempoFastMaxBpm)) {
        double faster_score = -1.0e300;
        for (int bpm = faster - kTempoBpmQuantum; bpm <= faster + kTempoBpmQuantum; ++bpm) {
            if (bpm >= min_bpm && bpm <= max_bpm) {
                faster_score = std::max(faster_score, scores[static_cast<std::size_t>(bpm)]);
            }
        }
        if (faster_score >= best * static_cast<double>(kTempoDoubleFit)) {
            chosen = faster;
        }
    }
    chosen = quantise_bpm(chosen, min_bpm, max_bpm);
    return std::clamp(60000 / chosen, kMinBeatMs, kMaxBeatMs);
}

struct Peak {
    std::int32_t ms = 0;
    float strength = 0.f;
    float low_share = 0.5f;
    float centroid = 0.f;
    float sustain = 0.f;
    float pitch_delta = 0.f;
    float rise = 0.f;
    float low_flux = 0.f;
    float high_flux = 0.f;
    float low_centroid = 0.f;
    bool strong = false;
};

std::vector<Peak> find_peaks(const OnsetAnalysis& analysis) {
    std::vector<Peak> peaks;
    const int n = static_cast<int>(analysis.envelope.size());
    if (n < 3) {
        return peaks;
    }
    float global = 0.f;
    for (const float value : analysis.envelope) {
        global = std::max(global, value);
    }
    if (!(global > 0.f)) {
        return peaks;
    }
    const double step = hop_ms(analysis);
    const int later_frames = std::max(1, static_cast<int>(std::lround(80.0 / step)));
    for (int i = 1; i + 1 < n; ++i) {
        const float value = analysis.envelope[static_cast<std::size_t>(i)];
        if (!(value >= analysis.envelope[static_cast<std::size_t>(i - 1)]) ||
            !(value > analysis.envelope[static_cast<std::size_t>(i + 1)]) || !(value > 0.f)) {
            continue;
        }
        const int lo = std::max(0, i - kAdaptFrames);
        const int hi = std::min(n, i + kAdaptFrames + 1);
        std::vector<float> window(analysis.envelope.begin() + lo, analysis.envelope.begin() + hi);
        const auto mid = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
        std::nth_element(window.begin(), mid, window.end());
        const float med = *mid;
        for (float& sample : window) {
            sample = std::fabs(sample - med);
        }
        std::nth_element(window.begin(), mid, window.end());
        const float scale = std::max(*mid, global * 0.02f);
        const bool strong = value >= med + kStrongMad * scale && value >= kStrongFloor * global;
        const bool weak = value >= med + kWeakMad * scale && value >= kWeakFloor * global;
        if (!strong && !weak) {
            continue;
        }
        const float y0 = analysis.envelope[static_cast<std::size_t>(i - 1)];
        const float y2 = analysis.envelope[static_cast<std::size_t>(i + 1)];
        const float denom = y0 - 2.f * value + y2;
        float delta = 0.f;
        if (std::fabs(denom) > 1e-8f) {
            delta = std::clamp(0.5f * (y0 - y2) / denom, -0.5f, 0.5f);
        }
        Peak peak;
        peak.ms = time_of(static_cast<double>(i) + static_cast<double>(delta), analysis);
        peak.strength = value;
        peak.strong = strong;
        peak.low_share = analysis.low_share[static_cast<std::size_t>(i)];
        peak.centroid = analysis.centroid_hz[static_cast<std::size_t>(i)];
        peak.low_flux = analysis.low_flux[static_cast<std::size_t>(i)];
        peak.high_flux = analysis.high_flux[static_cast<std::size_t>(i)];
        peak.low_centroid = analysis.low_centroid_hz[static_cast<std::size_t>(i)];
        const int after = std::min(n - 1, i + later_frames);
        const float later = analysis.energy[static_cast<std::size_t>(after)];
        peak.sustain = later / (analysis.energy[static_cast<std::size_t>(i)] + 6.f * value + 1.f);
        const int prev = std::max(0, i - 1);
        const int next = std::min(n - 1, i + 1);
        peak.pitch_delta = analysis.centroid_hz[static_cast<std::size_t>(next)] -
                           analysis.centroid_hz[static_cast<std::size_t>(prev)];
        peak.rise = rise_across(analysis, i);
        if (!peaks.empty() && peak.ms <= peaks.back().ms + kOnsetMergeMs) {
            if (peak.strength > peaks.back().strength) {
                peaks.back() = peak;
            }
            continue;
        }
        peaks.push_back(peak);
    }
    return peaks;
}

int nearest_peak(const std::vector<Peak>& peaks, std::int32_t ms) {
    if (peaks.empty()) {
        return -1;
    }
    const auto by_time = [](const Peak& peak, std::int32_t time) { return peak.ms < time; };
    const auto it = std::lower_bound(peaks.begin(), peaks.end(), ms, by_time);
    const int right = static_cast<int>(it - peaks.begin());
    const int n = static_cast<int>(peaks.size());
    int best = -1;
    int best_dt = kOnsetMergeMs + 1;
    for (int index = right; index < n; ++index) {
        const Peak& peak = peaks[static_cast<std::size_t>(index)];
        if (peak.ms - ms > kOnsetMergeMs) {
            break;
        }
        const int dt = peak.ms - ms;
        if (dt < best_dt) {
            best_dt = dt;
            best = index;
        }
    }
    for (int index = right - 1; index >= 0; --index) {
        const Peak& peak = peaks[static_cast<std::size_t>(index)];
        if (ms - peak.ms > kOnsetMergeMs) {
            break;
        }
        const int dt = ms - peak.ms;
        if (dt < best_dt) {
            best_dt = dt;
            best = index;
        }
    }
    return best;
}

struct Cand {
    std::int32_t ms = 0;
    float strength = 0.f;
    int peak = -1;
    int tier = 0;
};

std::vector<Cand> build_candidates(const OnsetAnalysis& analysis,
                                   const std::vector<Peak>& peaks,
                                   int period_ms,
                                   std::int32_t start,
                                   std::int32_t end) {
    std::vector<Cand> cands;
    cands.reserve(peaks.size() * 2u);
    for (int i = 0; i < static_cast<int>(peaks.size()); ++i) {
        const Peak& peak = peaks[static_cast<std::size_t>(i)];
        if (peak.ms < start || peak.ms >= end) {
            continue;
        }
        Cand cand;
        cand.ms = peak.ms;
        cand.strength = peak.strength;
        cand.peak = i;
        cand.tier = peak.strong ? 0 : 2;
        cands.push_back(cand);
    }
    std::vector<float> periods(analysis.envelope.size(), static_cast<float>(period_ms));
    const std::vector<std::int32_t> beats =
        track_beats(analysis.envelope, periods, analysis.hop_frames, analysis.sample_rate);
    const auto add_grid = [&](std::int32_t ms, int tier, float scale) {
        if (ms < start || ms >= end || nearest_peak(peaks, ms) >= 0) {
            return;
        }
        Cand cand;
        cand.ms = ms;
        cand.strength = envelope_at(ms, analysis) * scale;
        cand.tier = tier;
        cands.push_back(cand);
    };
    for (const std::int32_t beat : beats) {
        add_grid(beat, 1, 0.9f);
        add_grid(beat + period_ms / 2, 3, 0.45f);
        add_grid(beat + period_ms / 4, 4, 0.25f);
        add_grid(beat + (3 * period_ms) / 4, 4, 0.25f);
    }
    return cands;
}

bool gap_ok(const std::vector<std::int32_t>& chosen, std::int32_t ms, int min_gap_ms) {
    const int gap = min_gap_ms > 0 ? min_gap_ms : kMinGapMs;
    const auto it = std::lower_bound(chosen.begin(), chosen.end(), ms);
    if (it != chosen.end() && *it - ms < gap) {
        return false;
    }
    if (it != chosen.begin() && ms - *(it - 1) < gap) {
        return false;
    }
    return true;
}

// Strong peaks, then weak peaks, then the beat grid. A lower rank is taken first.
int fill_rank(int tier) {
    switch (tier) {
    case 0:
        return 0;
    case 2:
        return 1;
    case 1:
        return 2;
    case 3:
        return 3;
    default:
        return 4;
    }
}

// Each absolute window keeps its own quota. A missing candidate shortens that
// window only. Bare beats are skipped once they would put fewer than about
// three quarters of the hits on an onset.
std::vector<Cand> select_hits(const std::vector<Cand>& cands,
                              int target,
                              std::int32_t start,
                              std::int32_t end,
                              int min_gap_ms) {
    std::vector<Cand> chosen;
    const int gap = min_gap_ms > 0 ? min_gap_ms : kMinGapMs;
    if (target < 1 || cands.empty() || end - start < gap) {
        return chosen;
    }
    const int window = std::max(kSelectWindowMs, gap);
    const int first = start / window;
    const int last = (end - 1) / window;
    struct Span {
        std::int32_t lo = 0;
        std::int32_t hi = 0;
        double remain = 0.0;
        int quota = 0;
    };
    std::vector<Span> spans;
    double span_sum = 0.0;
    for (int w = first; w <= last; ++w) {
        Span span;
        span.lo = std::max(start, w * window);
        span.hi = std::min(end, (w + 1) * window);
        if (span.hi <= span.lo) {
            continue;
        }
        span.remain = static_cast<double>(span.hi - span.lo);
        span_sum += span.remain;
        spans.push_back(span);
    }
    if (spans.empty() || !(span_sum > 0.0)) {
        return chosen;
    }
    int assigned = 0;
    for (Span& span : spans) {
        const double exact = static_cast<double>(target) * span.remain / span_sum;
        span.quota = static_cast<int>(std::floor(exact));
        span.remain = exact - static_cast<double>(span.quota);
        assigned += span.quota;
    }
    while (assigned < target) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < spans.size(); ++i) {
            const bool higher = spans[i].remain > spans[best].remain + 1.0e-9;
            const bool tie = std::fabs(spans[i].remain - spans[best].remain) <= 1.0e-9 &&
                             spans[i].lo < spans[best].lo;
            if (higher || tie) {
                best = i;
            }
        }
        if (!(spans[best].remain > 0.0)) {
            std::size_t longest = best;
            for (std::size_t i = 0; i < spans.size(); ++i) {
                if (spans[i].hi - spans[i].lo > spans[longest].hi - spans[longest].lo) {
                    longest = i;
                }
            }
            ++spans[longest].quota;
            ++assigned;
            continue;
        }
        ++spans[best].quota;
        spans[best].remain = -1.0;
        ++assigned;
    }

    float loudest = 0.f;
    for (const Cand& cand : cands) {
        loudest = std::max(loudest, cand.strength);
    }
    const auto level_of = [&](float strength) {
        if (!(loudest > 0.f)) {
            return 0;
        }
        if (strength >= loudest * 0.30f) {
            return 2;
        }
        if (strength >= loudest * 0.12f) {
            return 1;
        }
        return 0;
    };
    // Same rank: a weak peak yields to a stronger weak peak. Other tiers keep
    // the coarse loudness buckets, then the earlier time.
    const auto preferred = [&](int index, int best) {
        if (best < 0) {
            return true;
        }
        const Cand& cand = cands[static_cast<std::size_t>(index)];
        const Cand& other = cands[static_cast<std::size_t>(best)];
        const int rank = fill_rank(cand.tier);
        const int other_rank = fill_rank(other.tier);
        if (rank != other_rank) {
            return rank < other_rank;
        }
        if (cand.tier == 2 && std::fabs(cand.strength - other.strength) > 1.0e-6f) {
            return cand.strength > other.strength;
        }
        const int level = level_of(cand.strength);
        const int other_level = level_of(other.strength);
        if (cand.tier != 2 && level != other_level) {
            return level > other_level;
        }
        return cand.ms < other.ms;
    };
    const auto accept =
        [&](const Cand& cand, const std::vector<std::int32_t>& times, int onset_count) {
            if (!gap_ok(times, cand.ms, gap)) {
                return false;
            }
            if (cand.peak >= 0) {
                return true;
            }
            const int next_total = static_cast<int>(times.size()) + 1;
            return onset_count * 4 >= next_total * 3;
        };
    const auto insert_cand =
        [&](const Cand& cand, std::vector<std::int32_t>& times, int& onset_count) {
            const auto it = std::lower_bound(times.begin(), times.end(), cand.ms);
            const auto offset = static_cast<std::size_t>(it - times.begin());
            times.insert(it, cand.ms);
            chosen.insert(chosen.begin() + static_cast<std::ptrdiff_t>(offset), cand);
            if (cand.peak >= 0) {
                ++onset_count;
            }
        };

    std::vector<std::int32_t> times;
    chosen.reserve(static_cast<std::size_t>(target));
    int onset_count = 0;
    for (const Span& span : spans) {
        if (span.quota < 1) {
            continue;
        }
        const double spacing =
            static_cast<double>(span.hi - span.lo) / static_cast<double>(span.quota);
        for (int slot = 0; slot < span.quota; ++slot) {
            const double slo = static_cast<double>(span.lo) + static_cast<double>(slot) * spacing;
            const double shi = slot + 1 == span.quota ? static_cast<double>(span.hi)
                                                      : static_cast<double>(span.lo) +
                                                            static_cast<double>(slot + 1) * spacing;
            int best = -1;
            for (int index = 0; index < static_cast<int>(cands.size()); ++index) {
                const Cand& cand = cands[static_cast<std::size_t>(index)];
                if (static_cast<double>(cand.ms) < slo || static_cast<double>(cand.ms) >= shi ||
                    !accept(cand, times, onset_count)) {
                    continue;
                }
                if (preferred(index, best)) {
                    best = index;
                }
            }
            if (best < 0) {
                continue;
            }
            insert_cand(cands[static_cast<std::size_t>(best)], times, onset_count);
        }
    }
    // A full window does not take another hit. The shortfall stays in the
    // windows that missed: strong peaks, then weak peaks, then the beat grid.
    if (static_cast<int>(chosen.size()) < target) {
        std::vector<int> placed(spans.size(), 0);
        for (const Cand& cand : chosen) {
            for (int s = 0; s < static_cast<int>(spans.size()); ++s) {
                if (cand.ms >= spans[static_cast<std::size_t>(s)].lo &&
                    cand.ms < spans[static_cast<std::size_t>(s)].hi) {
                    ++placed[static_cast<std::size_t>(s)];
                    break;
                }
            }
        }
        for (int s = 0; s < static_cast<int>(spans.size()); ++s) {
            const Span& span = spans[static_cast<std::size_t>(s)];
            while (placed[static_cast<std::size_t>(s)] < span.quota &&
                   static_cast<int>(chosen.size()) < target) {
                int best = -1;
                for (int index = 0; index < static_cast<int>(cands.size()); ++index) {
                    const Cand& cand = cands[static_cast<std::size_t>(index)];
                    if (cand.ms < span.lo || cand.ms >= span.hi ||
                        !accept(cand, times, onset_count)) {
                        continue;
                    }
                    if (preferred(index, best)) {
                        best = index;
                    }
                }
                if (best < 0) {
                    break;
                }
                insert_cand(cands[static_cast<std::size_t>(best)], times, onset_count);
                ++placed[static_cast<std::size_t>(s)];
            }
        }
    }
    return chosen;
}

// One hit per equal slice of the song. Used when the experimental density asks
// for far fewer obstacles than the 2 second windows, which otherwise tie and
// pile the quota at the start.
std::vector<Cand> spread_slots(const std::vector<Cand>& cands,
                               int target,
                               std::int32_t start,
                               std::int32_t end,
                               int min_gap_ms) {
    std::vector<Cand> chosen;
    if (target < 1 || cands.empty() || end - start < min_gap_ms) {
        return chosen;
    }
    const double span = static_cast<double>(end - start) / static_cast<double>(target);
    std::vector<std::int32_t> times;
    for (int slot = 0; slot < target; ++slot) {
        const auto lo =
            static_cast<std::int32_t>(std::floor(static_cast<double>(start) + slot * span));
        const auto hi = static_cast<std::int32_t>(
            std::floor(static_cast<double>(start) + static_cast<double>(slot + 1) * span));
        int best = -1;
        for (int index = 0; index < static_cast<int>(cands.size()); ++index) {
            const Cand& cand = cands[static_cast<std::size_t>(index)];
            if (cand.ms < lo || cand.ms >= hi || !gap_ok(times, cand.ms, min_gap_ms)) {
                continue;
            }
            if (best < 0 || cand.strength > cands[static_cast<std::size_t>(best)].strength) {
                best = index;
            }
        }
        if (best < 0) {
            continue;
        }
        const Cand& cand = cands[static_cast<std::size_t>(best)];
        const auto it = std::lower_bound(times.begin(), times.end(), cand.ms);
        const auto offset = static_cast<std::size_t>(it - times.begin());
        times.insert(it, cand.ms);
        chosen.insert(chosen.begin() + static_cast<std::ptrdiff_t>(offset), cand);
    }
    return chosen;
}

struct Hit {
    std::int32_t ms = 0;
    float flux = 0.f;
    float low_share = 0.5f;
    float centroid = 0.f;
    float low_centroid = 0.f;
    float sustain = 0.f;
    float pitch_delta = 0.f;
    float rise = 0.f;
};

Hit hit_from(const Cand& cand, const std::vector<Peak>& peaks, const OnsetAnalysis& analysis) {
    Hit hit;
    hit.ms = cand.ms;
    hit.flux = cand.strength;
    if (cand.peak >= 0) {
        const Peak& peak = peaks[static_cast<std::size_t>(cand.peak)];
        hit.low_share = peak.low_share;
        hit.centroid = peak.centroid;
        hit.low_centroid = peak.low_centroid;
        hit.sustain = peak.sustain;
        hit.pitch_delta = peak.pitch_delta;
        hit.rise = peak.rise;
        hit.flux = std::max(hit.flux, peak.strength);
        return hit;
    }
    const int frame = frame_at(cand.ms, analysis);
    const std::size_t index = static_cast<std::size_t>(frame);
    hit.low_share = analysis.low_share[index];
    hit.centroid = analysis.centroid_hz[index];
    hit.low_centroid = analysis.low_centroid_hz[index];
    hit.flux = std::max(hit.flux, analysis.envelope[index]);
    const int after =
        std::min(static_cast<int>(analysis.energy.size()) - 1,
                 frame + std::max(1, static_cast<int>(std::lround(80.0 / hop_ms(analysis)))));
    hit.sustain = analysis.energy[static_cast<std::size_t>(after)] /
                  (analysis.energy[index] + 6.f * analysis.envelope[index] + 1.f);
    const int prev = std::max(0, frame - 1);
    const int next = std::min(static_cast<int>(analysis.centroid_hz.size()) - 1, frame + 1);
    hit.pitch_delta = analysis.centroid_hz[static_cast<std::size_t>(next)] -
                      analysis.centroid_hz[static_cast<std::size_t>(prev)];
    hit.rise = rise_across(analysis, frame);
    return hit;
}

// Types and pairs. A seeded sequence picks runs of 1–4. Audio can nudge a hit,
// and a repeated section copies an earlier phrase. The seed is multiplied by
// the tier's FSL course number so silver and gold are not the bronze list.
struct SeqRng {
    std::uint32_t state = 1;

    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    int below(int count) {
        if (count <= 1) {
            return 0;
        }
        // High bits: the low bits of this LCG repeat every few draws.
        return static_cast<int>(
            (static_cast<std::uint64_t>(next()) * static_cast<std::uint64_t>(count)) >> 32);
    }

    float unit() { return static_cast<float>(next() >> 8) * (1.f / 16777216.f); }
};

std::uint32_t sequence_seed(std::span<const std::int16_t> interleaved, Difficulty difficulty) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::int16_t sample : interleaved) {
        const auto value = static_cast<std::uint16_t>(sample);
        hash ^= value & 0xffu;
        hash *= 1099511628211ull;
        hash ^= value >> 8;
        hash *= 1099511628211ull;
    }
    // Disc courses reseed with random_seed * course_number. Bronze is course 1,
    // silver course 3, gold course 5.
    const auto tier = static_cast<std::uint32_t>(courses_for(difficulty).first) + 1u;
    auto seed = static_cast<std::uint32_t>(hash) * tier;
    if (seed == 0) {
        seed = tier == 0 ? 1u : tier;
    }
    return seed;
}

float pair_activity(const OnsetAnalysis& analysis) {
    if (analysis.envelope.empty()) {
        return 0.5f;
    }
    double sum = 0.0;
    int hot = 0;
    for (const float value : analysis.envelope) {
        sum += value;
        if (value > 2.f) {
            ++hot;
        }
    }
    const float mean = std::clamp(
        static_cast<float>(sum / static_cast<double>(analysis.envelope.size())) / 8.f, 0.f, 1.f);
    const float spikes = std::clamp(
        static_cast<float>(hot) / static_cast<float>(analysis.envelope.size()) / 0.20f, 0.f, 1.f);
    return 0.5f * (mean + spikes);
}

float pair_rate_for(const OnsetAnalysis& analysis) {
    const float rate = kPairFractionAim + (pair_activity(analysis) - 0.5f) * kPairActivitySwing;
    return std::clamp(rate, kPairFractionMin, kPairFractionMax);
}

int nudge_kind(const Hit& hit, SeqRng& rng, int rolled) {
    if (rng.unit() >= kTypeNudgeChance) {
        return rolled;
    }
    if (hit.low_share >= 0.80f) {
        return rng.below(2);
    }
    if (hit.centroid >= 2800.f) {
        return 2;
    }
    if (hit.sustain >= 0.55f) {
        return 3;
    }
    return rolled;
}

void break_strict_alternation(std::vector<int>& kinds) {
    const int n = static_cast<int>(kinds.size());
    if (n < 6) {
        return;
    }
    for (int i = 1; i < n; ++i) {
        if (kinds[static_cast<std::size_t>(i)] == kinds[static_cast<std::size_t>(i - 1)]) {
            return;
        }
    }
    kinds[1] = kinds[0];
}

void ensure_four_types(std::vector<int>& kinds, const std::vector<char>& protect) {
    const int n = static_cast<int>(kinds.size());
    if (n < 8) {
        return;
    }
    int tally[4] = {};
    for (const int kind : kinds) {
        ++tally[kind];
    }
    for (int missing = 0; missing < 4; ++missing) {
        if (tally[missing] > 0) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            if (!protect.empty() && protect[static_cast<std::size_t>(i)] != 0) {
                continue;
            }
            const int donor = kinds[static_cast<std::size_t>(i)];
            if (tally[donor] < 2) {
                continue;
            }
            kinds[static_cast<std::size_t>(i)] = missing;
            --tally[donor];
            ++tally[missing];
            break;
        }
    }
}

void cap_type_runs(std::vector<int>& kinds, const std::vector<char>& protect = {}) {
    const int n = static_cast<int>(kinds.size());
    for (int guard = 0; guard < n; ++guard) {
        int run_start = 0;
        int run = 1;
        bool shortened = false;
        for (int i = 1; i < n; ++i) {
            if (kinds[static_cast<std::size_t>(i)] == kinds[static_cast<std::size_t>(i - 1)]) {
                ++run;
            } else {
                run_start = i;
                run = 1;
            }
            if (run <= kTypeRunMax) {
                continue;
            }
            int at = i;
            if (!protect.empty() && protect[static_cast<std::size_t>(at)] != 0 && run_start > 0 &&
                (protect[static_cast<std::size_t>(run_start - 1)] == 0)) {
                at = run_start - 1;
            }
            kinds[static_cast<std::size_t>(at)] = (kinds[static_cast<std::size_t>(at)] + 1) % 4;
            shortened = true;
            break;
        }
        if (!shortened) {
            return;
        }
    }
}

std::vector<int> sequence_kinds(const std::vector<Hit>& hits, SeqRng& rng) {
    const int n = static_cast<int>(hits.size());
    std::vector<int> kinds(static_cast<std::size_t>(n), 0);
    int cursor = 0;
    int previous = -1;
    while (cursor < n) {
        int run = kTypeRunMin + rng.below(kTypeRunMax - kTypeRunMin + 1);
        if (run == 1 && previous >= 0 && rng.unit() < 0.40f) {
            run = 2;
        }
        if (cursor + run > n) {
            run = n - cursor;
        }
        int kind = nudge_kind(hits[static_cast<std::size_t>(cursor)], rng, rng.below(4));
        if (kind == previous) {
            kind = (kind + 1 + rng.below(3)) % 4;
        }
        for (int step = 0; step < run; ++step) {
            kinds[static_cast<std::size_t>(cursor + step)] = kind;
        }
        previous = kind;
        cursor += run;
    }
    break_strict_alternation(kinds);
    cap_type_runs(kinds);
    ensure_four_types(kinds, {});
    cap_type_runs(kinds);
    return kinds;
}

std::vector<char> sequence_pairs(int count, float rate, SeqRng& rng) {
    std::vector<char> paired(static_cast<std::size_t>(std::max(count, 0)), 0);
    if (count <= 0) {
        return paired;
    }
    int want = static_cast<int>(std::lround(static_cast<double>(rate) * count));
    want = std::clamp(want, 0, count);
    for (int placed = 0; placed < want; ++placed) {
        const double pos = (static_cast<double>(placed) + 0.30) * static_cast<double>(count) / want;
        int index = static_cast<int>(std::lround(pos)) + rng.below(3) - 1;
        index = std::clamp(index, 0, count - 1);
        for (int guard = 0; guard < count && paired[static_cast<std::size_t>(index)] != 0;
             ++guard) {
            index = (index + 1) % count;
        }
        paired[static_cast<std::size_t>(index)] = 1;
    }
    return paired;
}

float feature_cosine(const float* left, const float* right, int count) {
    double dot = 0.0;
    double left_energy = 0.0;
    double right_energy = 0.0;
    for (int i = 0; i < count; ++i) {
        dot += static_cast<double>(left[i]) * right[i];
        left_energy += static_cast<double>(left[i]) * left[i];
        right_energy += static_cast<double>(right[i]) * right[i];
    }
    if (left_energy < 1.0e-6 || right_energy < 1.0e-6) {
        return 0.f;
    }
    return static_cast<float>(dot / std::sqrt(left_energy * right_energy));
}

void reuse_repeated_phrases(std::vector<int>& kinds,
                            std::vector<char>& paired,
                            const std::vector<Hit>& hits,
                            const OnsetAnalysis& analysis,
                            int period_ms) {
    const int n = static_cast<int>(hits.size());
    if (n < kPhraseObstaclesMin * 2 || period_ms < kMinBeatMs || analysis.envelope.empty()) {
        return;
    }
    const int beats = std::max(4, (kPhraseWindowMs + period_ms - 1) / period_ms);
    const int window_ms = beats * period_ms;
    const std::int32_t last_ms = hits.back().ms;
    struct Win {
        std::int32_t lo = 0;
        std::int32_t hi = 0;
        int begin = 0;
        int end = 0;
        float feat[8] = {};
    };
    std::vector<Win> windows;
    for (std::int32_t lo = 0; lo + window_ms <= last_ms + window_ms / 4; lo += window_ms) {
        Win window;
        window.lo = lo;
        window.hi = lo + window_ms;
        while (window.begin < n && hits[static_cast<std::size_t>(window.begin)].ms < window.lo) {
            ++window.begin;
        }
        window.end = window.begin;
        while (window.end < n && hits[static_cast<std::size_t>(window.end)].ms < window.hi) {
            ++window.end;
        }
        float raw[8] = {};
        float mean = 0.f;
        for (int bin = 0; bin < 8; ++bin) {
            const std::int32_t bin_lo = window.lo + bin * window_ms / 8;
            const std::int32_t bin_hi = window.lo + (bin + 1) * window_ms / 8;
            const int first = frame_at(bin_lo, analysis);
            const int last = std::max(first + 1, frame_at(bin_hi, analysis));
            double sum = 0.0;
            int samples = 0;
            for (int frame = first;
                 frame < last && frame < static_cast<int>(analysis.envelope.size());
                 ++frame) {
                sum += analysis.envelope[static_cast<std::size_t>(frame)];
                ++samples;
            }
            raw[bin] = samples > 0 ? static_cast<float>(sum / samples) : 0.f;
            mean += raw[bin];
        }
        mean /= 8.f;
        for (int bin = 0; bin < 8; ++bin) {
            window.feat[bin] = raw[bin] - mean;
        }
        windows.push_back(window);
    }
    std::vector<char> protect(static_cast<std::size_t>(n), 0);
    for (int i = 1; i < static_cast<int>(windows.size()); ++i) {
        int best = -1;
        float best_sim = kPhraseSimilarity;
        for (int earlier = 0; earlier < i; ++earlier) {
            const int earlier_count = windows[static_cast<std::size_t>(earlier)].end -
                                      windows[static_cast<std::size_t>(earlier)].begin;
            if (earlier_count < kPhraseObstaclesMin) {
                continue;
            }
            const float sim = feature_cosine(windows[static_cast<std::size_t>(earlier)].feat,
                                             windows[static_cast<std::size_t>(i)].feat,
                                             8);
            if (sim > best_sim) {
                best_sim = sim;
                best = earlier;
            }
        }
        if (best < 0) {
            continue;
        }
        const Win& source = windows[static_cast<std::size_t>(best)];
        const Win& dest = windows[static_cast<std::size_t>(i)];
        int phrase =
            std::clamp(source.end - source.begin, kPhraseObstaclesMin, kPhraseObstaclesMax);
        phrase = std::min(phrase, dest.end - dest.begin);
        if (phrase < kPhraseObstaclesMin) {
            continue;
        }
        for (int step = 0; step < phrase; ++step) {
            const int from = source.begin + step;
            const int to = dest.begin + step;
            kinds[static_cast<std::size_t>(to)] = kinds[static_cast<std::size_t>(from)];
            paired[static_cast<std::size_t>(to)] = paired[static_cast<std::size_t>(from)];
            protect[static_cast<std::size_t>(to)] = 1;
        }
    }
    cap_type_runs(kinds, protect);
    ensure_four_types(kinds, protect);
    cap_type_runs(kinds, protect);
}

void fft_inplace(std::vector<std::complex<double>>& data) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * 3.141592653589793 / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// Mean spectral flatness of energetic frames. Steady noise sits high; a tone sits low.
float mean_flatness(const Prepared& audio) {
    const int window = audio.window >= 16 ? audio.window : 256;
    if (audio.frames < window || audio.pcm.size() < static_cast<std::size_t>(window) * 2u) {
        return 0.f;
    }
    const int hop = std::max(window / 2, audio.hop);
    const int bins = window / 2;
    std::vector<double> hann(static_cast<std::size_t>(window));
    for (int i = 0; i < window; ++i) {
        hann[static_cast<std::size_t>(i)] =
            0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / (window - 1));
    }
    double flat_sum = 0.0;
    int frames = 0;
    int active = 0;
    std::vector<std::complex<double>> buffer(static_cast<std::size_t>(window));
    for (int start = 0; start + window <= audio.frames; start += hop * 4) {
        for (int i = 0; i < window; ++i) {
            const std::size_t frame = static_cast<std::size_t>(start + i);
            const double mono = (static_cast<double>(audio.pcm[frame * 2u]) +
                                 static_cast<double>(audio.pcm[frame * 2u + 1u])) /
                                65536.0;
            buffer[static_cast<std::size_t>(i)] =
                std::complex<double>(mono * hann[static_cast<std::size_t>(i)], 0.0);
        }
        fft_inplace(buffer);
        double arith = 0.0;
        double log_sum = 0.0;
        int used = 0;
        for (int bin = 1; bin < bins; ++bin) {
            const double mag = std::abs(buffer[static_cast<std::size_t>(bin)]) + 1.0e-12;
            arith += mag;
            log_sum += std::log(mag);
            ++used;
        }
        ++frames;
        if (used < 4 || !(arith > 0.02)) {
            continue;
        }
        const double geo = std::exp(log_sum / used);
        flat_sum += geo / (arith / used);
        ++active;
    }
    if (frames < 4 || active * 5 < frames * 3) {
        return 0.f;
    }
    return static_cast<float>(flat_sum / active);
}

bool steady_noise(const Prepared& audio, const OnsetAnalysis& analysis, float loudest_flux) {
    // The onset envelope is a half-wave residual, so its median is ~0 even for
    // noise. Spectral energy stays high across a steady bed, and flatness
    // separates that bed from a tone.
    if (analysis.energy.size() < 16 || loudest_flux < kTonalPeakFlux) {
        return false;
    }
    std::vector<float> env = analysis.energy;
    const float med = median_of(env);
    if (!(med > 0.01f)) {
        return false;
    }
    const std::size_t rank = env.size() * 95 / 100;
    std::nth_element(env.begin(), env.begin() + static_cast<std::ptrdiff_t>(rank), env.end());
    const float prom = env[rank] / med;
    if (!(prom < 8.f)) {
        return false;
    }
    // Compressed music is as flat and as steady as noise, but its drums and
    // attacks stand out of the onset flux. A noise bed has no such peaks.
    std::vector<float> flux = analysis.envelope;
    const std::size_t typical = flux.size() * 90 / 100;
    const std::size_t top = flux.size() * 99 / 100;
    std::nth_element(flux.begin(), flux.begin() + static_cast<std::ptrdiff_t>(top), flux.end());
    const float top_flux = flux[top];
    std::nth_element(flux.begin(), flux.begin() + static_cast<std::ptrdiff_t>(typical), flux.end());
    const float typical_flux = flux[typical];
    if (!(typical_flux > 0.f) || top_flux >= kNoiseOnsetContrast * typical_flux) {
        return false;
    }
    return mean_flatness(audio) >= 0.35f;
}

float experimental_density(float onsets_per_second) {
    constexpr float kLow = kExperimentalDensityKneeLow;
    constexpr float kHigh = kExperimentalDensityKneeHigh;
    float t = 0.f;
    if (kHigh > kLow) {
        t = std::clamp((onsets_per_second - kLow) / (kHigh - kLow), 0.f, 1.f);
    }
    const float smooth = t * t * (3.f - 2.f * t);
    return kExperimentalDensityMin + (kExperimentalDensityMax - kExperimentalDensityMin) * smooth;
}

// Open while the short-term peak is at least -20 dBFS, and stay open down to
// -24. A click is judged by its peak, not by its RMS across the gap after it.
// The window is centered on the time it labels, so a short attack is open at
// the attack. A near-silent run (RMS) longer than 8 s is closed when that gate is on.
std::vector<char>
placement_mask(std::span<const std::int16_t> interleaved, int frames, bool loudness, bool silence) {
    std::vector<char> open;
    if (frames <= 0 || interleaved.size() < static_cast<std::size_t>(frames) * 2u ||
        (!loudness && !silence)) {
        return open;
    }
    const int hop = std::max(1, kCddaRate / 100);
    const int window = std::max(hop, kCddaRate * kLoudnessWindowMs / 1000);
    const int steps = (frames + hop - 1) / hop;
    // Closed until a centered window crosses the open threshold. Silence-only
    // masks start open and the long-silence pass punches holes in them.
    open.assign(static_cast<std::size_t>(steps), loudness ? 0 : 1);
    std::vector<int> hop_peak(static_cast<std::size_t>(steps), 0);
    for (int i = 0; i < frames; ++i) {
        const int left = std::abs(static_cast<int>(interleaved[static_cast<std::size_t>(i) * 2u]));
        const int right =
            std::abs(static_cast<int>(interleaved[static_cast<std::size_t>(i) * 2u + 1u]));
        int& slot = hop_peak[static_cast<std::size_t>(i / hop)];
        slot = std::max(slot, std::max(left, right));
    }
    bool gate = false;
    double sum_sq = 0.0;
    int lo = 0;
    int end = 0;
    int silence_at = -1;
    std::vector<std::pair<int, int>> silent_spans;
    const int total_ms = static_cast<int>(static_cast<long long>(frames) * 1000 / kCddaRate);
    for (int step = 0; step < steps; ++step) {
        const int next = std::min(frames, end + hop);
        for (int i = end; i < next; ++i) {
            const double left = interleaved[static_cast<std::size_t>(i) * 2u];
            const double right = interleaved[static_cast<std::size_t>(i) * 2u + 1u];
            sum_sq += left * left + right * right;
        }
        end = next;
        const int start = std::max(0, end - window);
        while (lo < start) {
            const double left = interleaved[static_cast<std::size_t>(lo) * 2u];
            const double right = interleaved[static_cast<std::size_t>(lo) * 2u + 1u];
            sum_sq -= left * left + right * right;
            ++lo;
        }
        if (sum_sq < 0.0) {
            sum_sq = 0.0;
        }
        const int count = (end - lo) * 2;
        const double rms = count > 0 ? std::sqrt(sum_sq / static_cast<double>(count)) : 0.0;
        const double dbfs = rms > 1.0e-3 ? 20.0 * std::log10(rms / 32768.0) : -120.0;
        const int center = lo + (end - lo) / 2;
        const int ms = static_cast<int>(static_cast<long long>(center) * 1000 / kCddaRate);
        const int index = std::clamp(ms / 10, 0, steps - 1);
        if (loudness) {
            int peak = 0;
            for (int h = lo / hop; h <= (end - 1) / hop && h < steps; ++h) {
                peak = std::max(peak, hop_peak[static_cast<std::size_t>(h)]);
            }
            const double peak_dbfs =
                peak > 0 ? 20.0 * std::log10(static_cast<double>(peak) / 32768.0) : -120.0;
            if (!gate && peak_dbfs >= static_cast<double>(kLoudnessOpenDbfs)) {
                gate = true;
            } else if (gate && peak_dbfs < static_cast<double>(kLoudnessCloseDbfs)) {
                gate = false;
            }
            open[static_cast<std::size_t>(index)] = gate ? 1 : 0;
        }
        if (!silence) {
            continue;
        }
        if (dbfs < -50.0) {
            if (silence_at < 0) {
                silence_at = ms;
            }
        } else if (silence_at >= 0) {
            if (ms - silence_at >= kLongSilenceMs) {
                silent_spans.emplace_back(silence_at, ms);
            }
            silence_at = -1;
        }
    }
    if (silence && silence_at >= 0 && total_ms - silence_at >= kLongSilenceMs) {
        silent_spans.emplace_back(silence_at, total_ms);
    }
    for (const auto& span : silent_spans) {
        for (int step = 0; step < steps; ++step) {
            const int ms = step * (1000 * hop / kCddaRate);
            if (ms >= span.first && ms < span.second) {
                open[static_cast<std::size_t>(step)] = 0;
            }
        }
    }
    return open;
}

bool mask_open(const std::vector<char>& mask, std::int32_t ms) {
    if (mask.empty()) {
        return true;
    }
    if (ms < 0) {
        return false;
    }
    const int index = ms / 10;
    if (index >= static_cast<int>(mask.size())) {
        return mask.back() != 0;
    }
    return mask[static_cast<std::size_t>(index)] != 0;
}

bool mask_any_open(const std::vector<char>& mask, std::int32_t start, std::int32_t end) {
    if (mask.empty()) {
        return true;
    }
    const int lo = std::clamp(start / 10, 0, static_cast<int>(mask.size()));
    const int hi = std::clamp(end / 10, 0, static_cast<int>(mask.size()));
    for (int i = lo; i < hi; ++i) {
        if (mask[static_cast<std::size_t>(i)] != 0) {
            return true;
        }
    }
    return false;
}

// Snaps each hit to its nearest tracked beat and keeps one only when it is at
// least the stride floor after the previous one: kExperimentalMinGapMs and
// kExperimentalStrideMinBeats, whichever is longer. The hits come from equal
// time slots, so the gaps vary inside the 4–7 beat stride and a re-encode
// that moves one onset cannot shift every later hit.
std::vector<Hit>
space_on_stride(std::vector<Hit> hits, const OnsetAnalysis& analysis, int period_ms) {
    if (hits.size() < 2 || period_ms < 1) {
        return hits;
    }
    std::vector<float> periods(analysis.envelope.size(), static_cast<float>(period_ms));
    const std::vector<std::int32_t> beats =
        track_beats(analysis.envelope, periods, analysis.hop_frames, analysis.sample_rate);
    const int floor_beats =
        std::max(kExperimentalStrideMinBeats, (kExperimentalMinGapMs + period_ms - 1) / period_ms);
    // A tracked beat may run a few milliseconds short of the exact grid.
    const int floor_ms = std::max(kExperimentalMinGapMs, floor_beats * period_ms - kOnsetMergeMs);
    std::vector<Hit> kept;
    for (Hit hit : hits) {
        if (!beats.empty()) {
            const auto it = std::lower_bound(beats.begin(), beats.end(), hit.ms);
            std::int32_t best = hit.ms;
            int best_dt = period_ms / 2 + 1;
            if (it != beats.end() && *it - hit.ms < best_dt) {
                best = *it;
                best_dt = *it - hit.ms;
            }
            if (it != beats.begin() && hit.ms - *(it - 1) < best_dt) {
                best = *(it - 1);
            }
            hit.ms = best;
        }
        if (!kept.empty() && hit.ms - kept.back().ms < floor_ms) {
            continue;
        }
        if (!kept.empty()) {
            hit.ms = std::max(hit.ms, kept.back().ms + kExperimentalMinGapMs);
        }
        kept.push_back(hit);
    }
    return kept;
}

std::vector<std::int32_t>
loop_grid(std::int32_t start, std::int32_t end, int target, int min_gap_ms) {
    std::vector<std::int32_t> hits;
    const int gap = min_gap_ms > 0 ? min_gap_ms : kMinGapMs;
    if (target < 1 || end - start < gap) {
        return hits;
    }
    const double spacing = static_cast<double>(end - start) / static_cast<double>(target);
    std::int32_t previous = start - gap;
    for (int i = 0; i < target; ++i) {
        auto hit = static_cast<std::int32_t>(
            std::llround(static_cast<double>(start) + (static_cast<double>(i) + 0.5) * spacing));
        if (hit < previous + gap) {
            hit = previous + gap;
        }
        if (hit >= end) {
            break;
        }
        hits.push_back(hit);
        previous = hit;
    }
    return hits;
}

std::vector<float> envelope_on_step(const OnsetAnalysis& analysis) {
    const std::vector<float>& env = analysis.envelope;
    if (env.empty()) {
        return {};
    }
    if (static_cast<long long>(analysis.hop_frames) * 1000 ==
        static_cast<long long>(analysis.sample_rate) * kOnsetStepMs) {
        return env;
    }
    const double src_ms = hop_ms(analysis);
    const double end_ms = src_ms * static_cast<double>(env.size() - 1);
    const int out_n = static_cast<int>(end_ms / static_cast<double>(kOnsetStepMs)) + 1;
    std::vector<float> out(static_cast<std::size_t>(std::max(out_n, 0)), 0.f);
    const int n = static_cast<int>(env.size());
    for (int i = 0; i < out_n; ++i) {
        const double pos = (static_cast<double>(i) * kOnsetStepMs) / src_ms;
        const int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, n - 1);
        const int i1 = std::min(i0 + 1, n - 1);
        const float frac = static_cast<float>(pos - std::floor(pos));
        out[static_cast<std::size_t>(i)] = env[static_cast<std::size_t>(i0)] * (1.f - frac) +
                                           env[static_cast<std::size_t>(i1)] * frac;
    }
    return out;
}

} // namespace

std::vector<std::int32_t>
music_beat_grid(std::span<const std::int32_t> beats, int period_ms, std::int32_t end_ms) {
    if (period_ms < 1) {
        period_ms = kDefaultMusicBeatMs;
    }
    const int quarter = std::max(1, period_ms / 4);
    const int half = period_ms / 2;
    const int three = (3 * period_ms) / 4;
    const std::int64_t limit = static_cast<std::int64_t>(end_ms) + period_ms;
    std::vector<std::int32_t> grid;
    const auto add = [&](std::int64_t ms) {
        if (ms < 0 || ms > limit) {
            return;
        }
        grid.push_back(static_cast<std::int32_t>(ms));
    };
    if (beats.empty()) {
        const std::int32_t last = end_ms > 0 ? end_ms + period_ms : period_ms * 4;
        for (std::int32_t t = 0; t <= last; t += quarter) {
            add(t);
        }
    } else {
        for (const std::int32_t beat : beats) {
            add(beat);
            add(static_cast<std::int64_t>(beat) + quarter);
            add(static_cast<std::int64_t>(beat) + half);
            add(static_cast<std::int64_t>(beat) + three);
        }
    }
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
    return grid;
}

void align_perfect_centers(std::vector<CourseEvent>& events,
                           std::span<const std::int32_t> beats,
                           int period_ms) {
    if (events.empty()) {
        return;
    }
    if (period_ms < 1) {
        period_ms = kDefaultMusicBeatMs;
    }
    std::int32_t last = 0;
    for (const CourseEvent& event : events) {
        last = std::max(last, event.hit_ms);
    }
    // One extra period so a hit nudged forward to keep the gap still finds a point.
    const std::vector<std::int32_t> grid = music_beat_grid(beats, period_ms, last + period_ms);
    if (grid.empty()) {
        return;
    }
    std::int64_t previous = -1000000;
    for (CourseEvent& event : events) {
        const std::int64_t defined = event.hit_ms;
        // The snap mark is the chart hit plus the late bias, for every obstacle.
        // That is where perfect used to be centered. The window now ends at the
        // front edge, and a loop's front is left of the hit; neither changes
        // which hits move.
        const std::int64_t mark = defined + kJudgmentLateBiasMs;
        const std::int64_t shift = kJudgmentLateBiasMs;
        const std::int64_t earliest = previous + kMinGapMs;
        // The grid point has to be the beat that defines this hit, and the
        // mark has to already be near it. A neighboring quarter would pull
        // a downbeat attack off the pulse, and a swung onset stays put.
        std::int64_t target = -1;
        int best_dt = kPerfectBeatSnapMs + 1;
        for (const std::int32_t point : grid) {
            const int from_hit =
                static_cast<int>(std::llabs(static_cast<std::int64_t>(point) - defined));
            const int from_mark =
                static_cast<int>(std::llabs(static_cast<std::int64_t>(point) - mark));
            if (from_hit > kPerfectBeatSnapMs || from_mark > kPerfectBeatSnapMs ||
                from_mark >= best_dt) {
                continue;
            }
            const std::int64_t hit = static_cast<std::int64_t>(point) - shift;
            if (hit < earliest) {
                continue;
            }
            best_dt = from_mark;
            target = point;
        }
        if (target >= 0) {
            const std::int64_t hit = target - shift;
            event.hit_ms = static_cast<std::int32_t>(std::max<std::int64_t>(hit, 0));
        }
        previous = event.hit_ms;
    }
}

DensityBand density_band(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Bronze:
        return {kBronzeDensityMin, kBronzeDensityMax};
    case Difficulty::Silver:
        return {kSilverDensityMin, kSilverDensityMax};
    case Difficulty::Gold:
        return {kGoldDensityMin, kGoldDensityMax};
    }
    return {kBronzeDensityMin, kBronzeDensityMax};
}

float density_target(Difficulty difficulty) {
    const DensityBand band = density_band(difficulty);
    return 0.5f * (band.min + band.max);
}

float onset_limited_density(float onsets_per_second, Difficulty difficulty) {
    const DensityBand band = density_band(difficulty);
    const float aim = std::min(band.max, band.min + kDensityAimAboveMin);
    float rate = onsets_per_second;
    if (!(rate > 0.f)) {
        rate = 0.f;
    }
    const float span = kDensityKneeHigh - kDensityKneeLow;
    float t = 0.f;
    if (span > 0.f) {
        t = std::clamp((rate - kDensityKneeLow) / span, 0.f, 1.f);
    }
    const float smooth = t * t * (3.f - 2.f * t);
    const float floor_scale = kSparseDensityFloor + (1.f - kSparseDensityFloor) * smooth;
    const float lower = floor_scale * band.min;
    const float scaled = kOnsetDensityScale * rate;
    if (!(lower < aim)) {
        return aim;
    }
    return std::clamp(scaled, lower, aim);
}

float pair_fraction(Difficulty difficulty) {
    switch (difficulty) {
    case Difficulty::Bronze:
        return kBronzePairFraction;
    case Difficulty::Silver:
        return kSilverPairFraction;
    case Difficulty::Gold:
        return kGoldPairFraction;
    }
    return kBronzePairFraction;
}

int music_target_count(int duration_ms, Difficulty difficulty) {
    if (duration_ms <= 0) {
        return 0;
    }
    const float rate = density_target(difficulty);
    int count = static_cast<int>(std::lround(static_cast<double>(rate) * duration_ms / 1000.0));
    if (count < 1) {
        count = 1;
    }
    if (count > kMaxCourseEvents) {
        count = kMaxCourseEvents;
    }
    return count;
}

struct ChartListen {
    MusicProgress progress = nullptr;
    void* user = nullptr;
};

bool chart_progress(void* user, int hop, int hops) {
    auto* listen = static_cast<ChartListen*>(user);
    if (listen->progress == nullptr) {
        return true;
    }
    const float fraction = hops > 0 ? static_cast<float>(hop) / static_cast<float>(hops) : 1.f;
    return listen->progress(listen->user, fraction, "CHARTING");
}

Result<MusicChart> chart_music(std::span<const std::int16_t> interleaved,
                               int frames,
                               Difficulty difficulty,
                               MusicProgress progress,
                               void* progress_user,
                               MusicChartOptions options) {
    if (frames <= 0 || interleaved.size() < static_cast<std::size_t>(frames) * 2u) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }
    const Prepared prepared = decimate_music(interleaved, frames);
    ChartListen listen{progress, progress_user};
    const OnsetAnalysis analysis = analyze_onsets(prepared.pcm,
                                                  prepared.frames,
                                                  progress == nullptr ? nullptr : chart_progress,
                                                  &listen,
                                                  prepared.rate,
                                                  prepared.window,
                                                  prepared.hop);
    if (analysis.canceled) {
        return Result<MusicChart>::failure("closed");
    }
    if (analysis.energy.empty()) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }
    const float energy_median = median_of(analysis.energy);
    if (analysis.silent && !(energy_median > 0.01f)) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }

    double low_sum = 0.0;
    double env_sum = 0.0;
    for (std::size_t i = 0; i < analysis.envelope.size(); ++i) {
        env_sum += analysis.envelope[i];
        if (i < analysis.low_flux.size()) {
            low_sum += analysis.low_flux[i];
        }
    }
    // Low-band flux is what a lossy encode leaves alone. A bright track with
    // no low band falls back to the full envelope.
    const std::vector<float>& tempo_env =
        (low_sum > env_sum * 0.20 && low_sum > 1.0) ? analysis.low_flux : analysis.envelope;
    const int period_ms = estimate_period_ms(tempo_env, analysis.hop_frames, analysis.sample_rate);
    const std::int32_t duration_ms = cdda_duration_ms(frames);
    const std::int32_t approach = approach_for(period_ms, difficulty);
    const std::int32_t start = approach + kLeadInMs;
    const std::int32_t end = duration_ms;
    if (end - start < kMinGapMs) {
        return Result<MusicChart>::failure("that track is too short to chart");
    }

    const DensityBand band = density_band(difficulty);
    const double seconds = static_cast<double>(duration_ms) / 1000.0;
    int min_count = static_cast<int>(std::ceil(static_cast<double>(band.min) * seconds - 1.0e-9));
    int max_count = static_cast<int>(std::floor(static_cast<double>(band.max) * seconds + 1.0e-9));
    int target = music_target_count(duration_ms, difficulty);
    const int fit = static_cast<int>((end - start) / kMinGapMs);
    if (fit < 1) {
        return Result<MusicChart>::failure("that track is too short to chart");
    }
    if (max_count > fit) {
        max_count = fit;
    }
    if (min_count > max_count) {
        min_count = max_count;
    }
    if (min_count < 1) {
        min_count = 1;
    }
    target = std::clamp(target, min_count, std::max(min_count, max_count));

    const int skip = std::clamp(static_cast<int>(std::lround(300.0 / hop_ms(analysis))),
                                0,
                                static_cast<int>(analysis.envelope.size()));
    float loudest = 0.f;
    for (int i = skip; i < static_cast<int>(analysis.envelope.size()); ++i) {
        loudest = std::max(loudest, analysis.envelope[static_cast<std::size_t>(i)]);
    }
    const bool tonal = energy_median > 0.01f && loudest < kTonalPeakFlux;

    std::vector<Peak> peaks;
    double onset_rate = 0.0;
    if (!tonal) {
        peaks = find_peaks(analysis);
        // A peak the interpolator places a few milliseconds before the window
        // is still that window's first onset. Peaks closer than the merge
        // gap were already collapsed, so this cannot count one onset twice.
        const std::int32_t count_from = std::max(0, start - kOnsetMergeMs);
        const double playable = std::max(0.001, static_cast<double>(end - count_from) / 1000.0);
        int counted = 0;
        for (const Peak& peak : peaks) {
            if (peak.ms >= count_from && peak.ms < end) {
                ++counted;
            }
        }
        onset_rate = static_cast<double>(counted) / playable;
        const double limited =
            static_cast<double>(onset_limited_density(static_cast<float>(onset_rate), difficulty));
        int onset_target = static_cast<int>(std::lround(limited * seconds));
        if (limited + 1.0e-4 >= static_cast<double>(band.min)) {
            onset_target = std::clamp(onset_target, min_count, std::max(min_count, max_count));
        } else {
            onset_target = std::clamp(onset_target, 1, std::max(1, max_count));
        }
        target = std::min(onset_target, fit);
    }

    // Each placement gate is its own constant. The flag turns on any that are
    // still false, so promoting one is a one-line constant change.
    const bool track_density = kGenTrackDensity || options.experimental;
    const bool beat_stride = kGenBeatStride || options.experimental;
    const bool loudness = kGenLoudnessGate || options.experimental;
    const bool noise_gate = kGenSteadyNoise || options.experimental;
    const bool long_silence = kGenLongSilence || options.experimental;
    const bool noise = noise_gate && steady_noise(prepared, analysis, loudest);
    const std::vector<char> mask = placement_mask(interleaved, frames, loudness, long_silence);
    if ((loudness || long_silence) && !mask_any_open(mask, start, end)) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }
    if (noise) {
        target = static_cast<int>(
            std::lround(static_cast<double>(kExperimentalNoiseDensity) *
                        static_cast<double>(difficulty_density_scale(difficulty)) * seconds));
        target = std::clamp(target, 0, std::max(fit, 0));
    } else if (track_density) {
        // The rate counts only open time, so a gated stretch (silence, a quiet
        // intro) does not thin out the music around it.
        // Long silences are not part of the rate, so a gapped track is not
        // thinned out around them. The gaps between short clicks still count.
        double open_rate = onset_rate;
        const std::vector<char> silent = placement_mask(interleaved, frames, false, true);
        if (!silent.empty()) {
            const std::int32_t count_from = std::max(0, start - kOnsetMergeMs);
            int open_steps = 0;
            for (std::int32_t ms = count_from; ms < end; ms += 10) {
                open_steps += mask_open(silent, ms) ? 1 : 0;
            }
            int counted = 0;
            for (const Peak& peak : peaks) {
                if (peak.ms >= count_from && peak.ms < end && mask_open(mask, peak.ms)) {
                    ++counted;
                }
            }
            open_rate = static_cast<double>(counted) / std::max(1.0, open_steps / 100.0);
        }
        const float density = experimental_density(static_cast<float>(open_rate)) *
                              difficulty_density_scale(difficulty);
        target = static_cast<int>(std::lround(static_cast<double>(density) * seconds));
        target = std::clamp(target, 1, kMaxCourseEvents);
    }
    const int min_gap = beat_stride ? kExperimentalMinGapMs : kMinGapMs;
    if (beat_stride) {
        const int stride_fit = std::max(1, static_cast<int>((end - start) / kExperimentalMinGapMs));
        if (target > stride_fit) {
            target = stride_fit;
        }
    }

    std::vector<Hit> hits;
    // Noise has no musical peaks worth chasing, so the sparse target is spread
    // the same way as a tone. The onset picker would otherwise pile a small
    // quota at the start of a flat bed.
    if (tonal || noise) {
        for (const std::int32_t ms : loop_grid(start, end, target, min_gap)) {
            if (!mask_open(mask, ms)) {
                continue;
            }
            Hit hit;
            hit.ms = ms;
            hits.push_back(hit);
        }
    } else {
        std::vector<Cand> cands = build_candidates(analysis, peaks, period_ms, start, end);
        if (!mask.empty()) {
            std::erase_if(cands, [&](const Cand& cand) { return !mask_open(mask, cand.ms); });
        }
        const std::vector<Cand> chosen = track_density
                                             ? spread_slots(cands, target, start, end, min_gap)
                                             : select_hits(cands, target, start, end, min_gap);
        hits.reserve(chosen.size());
        for (const Cand& cand : chosen) {
            if (!mask_open(mask, cand.ms)) {
                continue;
            }
            hits.push_back(hit_from(cand, peaks, analysis));
        }
        if (beat_stride) {
            hits = space_on_stride(std::move(hits), analysis, period_ms);
            if (!mask.empty()) {
                std::erase_if(hits, [&](const Hit& hit) { return !mask_open(mask, hit.ms); });
            }
        }
    }
    if (hits.empty()) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }

    SeqRng rng{sequence_seed(interleaved, difficulty)};
    std::vector<int> kinds = sequence_kinds(hits, rng);
    std::vector<char> paired =
        sequence_pairs(static_cast<int>(hits.size()), pair_rate_for(analysis), rng);
    reuse_repeated_phrases(kinds, paired, hits, analysis, period_ms);

    MusicChart chart;
    chart.envelope = envelope_on_step(analysis);
    chart.timeline.track_index = 0;
    chart.timeline.cdda_track = 0;
    chart.timeline.events.reserve(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i) {
        CourseEvent event;
        event.hit_ms = hits[i].ms;
        event.approach_ms = approach;
        event.scroll_approach_ms = approach;
        event.beat_ms = period_ms;
        event.obstacle = obstacle_for(kinds[i], paired[i] != 0);
        chart.timeline.events.push_back(event);
    }
    // Perfect centers land on the generator's beat grid. Disc charts skip this
    // and leave attack stamps alone; the window center is that stamp plus the bias.
    std::vector<float> periods(analysis.envelope.size(), static_cast<float>(period_ms));
    const std::vector<std::int32_t> beats =
        track_beats(analysis.envelope, periods, analysis.hop_frames, analysis.sample_rate);
    align_perfect_centers(chart.timeline.events, beats, period_ms);
    keep_spawn_order(chart.timeline.events);
    drop_obstacles_after_audio(chart.timeline.events, duration_ms);
    if (chart.timeline.events.empty()) {
        return Result<MusicChart>::failure("that track has no rhythm to play");
    }
    const std::int32_t last = chart.timeline.events.back().hit_ms;
    chart.timeline.audio_end_ms = duration_ms;
    chart.timeline.duration_ms = std::max(duration_ms, last + kCourseEndTailMs);
    if (progress != nullptr) {
        progress(progress_user, 1.f, "CHARTING");
    }
    return Result<MusicChart>::success(std::move(chart));
}

int music_best(const MusicScoreBook& book, std::string_view fingerprint, Difficulty difficulty) {
    int best = 0;
    for (const MusicScore& row : book.rows) {
        if (row.fingerprint == fingerprint && row.difficulty == difficulty && row.score > best) {
            best = row.score;
        }
    }
    return best;
}

void music_note(MusicScoreBook& book,
                std::string_view fingerprint,
                Difficulty difficulty,
                int score) {
    for (MusicScore& row : book.rows) {
        if (row.fingerprint == fingerprint && row.difficulty == difficulty) {
            if (score > row.score) {
                row.score = score;
            }
            return;
        }
    }
    MusicScore row;
    row.fingerprint = std::string(fingerprint);
    row.difficulty = difficulty;
    row.score = score;
    book.rows.push_back(std::move(row));
}

Result<MusicScoreBook> load_music_scores(const std::filesystem::path& file) {
    MusicScoreBook book;
    if (file.empty()) {
        return Result<MusicScoreBook>::success(std::move(book));
    }
    std::ifstream in(file);
    if (!in) {
        return Result<MusicScoreBook>::success(std::move(book));
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        std::string fingerprint;
        std::string name;
        int score = 0;
        if (!(row >> fingerprint >> name >> score)) {
            continue;
        }
        Difficulty difficulty = Difficulty::Bronze;
        if (name == "silver") {
            difficulty = Difficulty::Silver;
        } else if (name == "gold") {
            difficulty = Difficulty::Gold;
        } else if (name != "bronze") {
            continue;
        }
        if (score < 0) {
            continue;
        }
        music_note(book, fingerprint, difficulty, score);
    }
    return Result<MusicScoreBook>::success(std::move(book));
}

Result<int> save_music_scores(const std::filesystem::path& file, const MusicScoreBook& book) {
    if (file.empty()) {
        return Result<int>::success(0);
    }
    std::error_code error;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            return Result<int>::failure("could not save music scores");
        }
    }
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return Result<int>::failure("could not save music scores");
    }
    out << "# oscilline music scores\n";
    for (const MusicScore& row : book.rows) {
        out << row.fingerprint << ' ' << difficulty_name(row.difficulty) << ' ' << row.score
            << '\n';
    }
    if (!out) {
        return Result<int>::failure("could not save music scores");
    }
    return Result<int>::success(0);
}

std::filesystem::path user_config_directory() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "oscilline";
    }
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "oscilline";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "oscilline";
    }
    return {};
}

std::filesystem::path music_score_path() {
    const std::filesystem::path dir = user_config_directory();
    if (dir.empty()) {
        return {};
    }
    return dir / "music-scores.txt";
}

} // namespace oscilline
