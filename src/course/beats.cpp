// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Spectral-flux onsets and a beat tracker locked to the script period.

#include "oscilline/course/beats.hpp"

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/mapping.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace oscilline {
namespace {

constexpr int kAverageFrames = 50; // half a second of envelope for the local mean
constexpr double kMagnitudeGain = 100.0;
constexpr double kPeriodPenalty = 4.0; // weight of the log-period deviation in the beat search

// In-place iterative radix-2 FFT. `data.size()` is a power of two.
void fft(std::vector<std::complex<double>>& data) {
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
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
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

bool hop_is_step(int hop_frames, int sample_rate) {
    return hop_frames > 0 && sample_rate > 0 &&
           static_cast<long long>(hop_frames) * 1000 ==
               static_cast<long long>(sample_rate) * kOnsetStepMs;
}

double frames_from_ms(double ms, int hop_frames, int sample_rate) {
    if (hop_is_step(hop_frames, sample_rate)) {
        return ms / static_cast<double>(kOnsetStepMs);
    }
    return ms * static_cast<double>(sample_rate) / (static_cast<double>(hop_frames) * 1000.0);
}

std::int32_t ms_from_frame(std::size_t index, int hop_frames, int sample_rate) {
    if (hop_is_step(hop_frames, sample_rate)) {
        return static_cast<std::int32_t>(index * static_cast<std::size_t>(kOnsetStepMs));
    }
    return static_cast<std::int32_t>((static_cast<unsigned long long>(index) *
                                      static_cast<unsigned long long>(hop_frames) * 1000ull) /
                                     static_cast<unsigned long long>(sample_rate));
}

} // namespace

OnsetAnalysis analyze_onsets(std::span<const std::int16_t> interleaved,
                             int frames,
                             OnsetProgress progress,
                             void* user,
                             int sample_rate,
                             int window_frames,
                             int hop_frames) {
    OnsetAnalysis analysis;
    if (sample_rate < 1000) {
        sample_rate = kCddaRate;
    }
    if (window_frames < 16 || (window_frames & (window_frames - 1)) != 0) {
        window_frames = kOnsetWindowFrames;
    }
    if (hop_frames < 1) {
        hop_frames = kOnsetHopFrames;
    }
    analysis.sample_rate = sample_rate;
    analysis.hop_frames = hop_frames;
    if (frames <= 0 || interleaved.size() < static_cast<std::size_t>(frames) * 2u ||
        frames < window_frames) {
        return analysis;
    }
    const std::size_t bins = static_cast<std::size_t>(window_frames) / 2u + 1u;
    const int low_bin = std::max(1, kOnsetLowHz * window_frames / sample_rate);
    const int high_bin = std::max(low_bin + 1, kOnsetHighHz * window_frames / sample_rate);
    std::vector<double> window(static_cast<std::size_t>(window_frames));
    for (int i = 0; i < window_frames; ++i) {
        window[static_cast<std::size_t>(i)] =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (window_frames - 1));
    }
    const int count = (frames - window_frames) / hop_frames + 1;
    std::vector<double> flux(static_cast<std::size_t>(count), 0.0);
    std::vector<double> low_flux_raw(static_cast<std::size_t>(count), 0.0);
    std::vector<double> high_flux_raw(static_cast<std::size_t>(count), 0.0);
    std::vector<double> previous(bins, 0.0);
    std::vector<std::complex<double>> buffer(static_cast<std::size_t>(window_frames));
    for (int f = 0; f < count; ++f) {
        const std::size_t start =
            static_cast<std::size_t>(f) * static_cast<std::size_t>(hop_frames);
        for (int i = 0; i < window_frames; ++i) {
            const std::size_t frame = start + static_cast<std::size_t>(i);
            const double mono = (static_cast<double>(interleaved[frame * 2u]) +
                                 static_cast<double>(interleaved[frame * 2u + 1u])) /
                                65536.0;
            buffer[static_cast<std::size_t>(i)] =
                std::complex<double>(mono * window[static_cast<std::size_t>(i)], 0.0);
        }
        fft(buffer);
        double sum = 0.0;
        double low_sum = 0.0;
        double high_sum = 0.0;
        double low_peak = 0.0;
        double high_peak = 0.0;
        double energy = 0.0;
        double mag_sum = 0.0;
        double weighted = 0.0;
        double low_mag = 0.0;
        double low_weighted = 0.0;
        for (std::size_t b = 0; b < bins; ++b) {
            const double magnitude = std::abs(buffer[b]);
            const double level = std::log1p(kMagnitudeGain * magnitude);
            if (f > 0 && level > previous[b]) {
                const double delta = level - previous[b];
                sum += delta;
                if (b > 0 && static_cast<int>(b) <= low_bin) {
                    low_sum = std::max(low_sum, delta);
                }
                if (static_cast<int>(b) >= high_bin) {
                    high_sum = std::max(high_sum, delta);
                }
            }
            previous[b] = level;
            energy += level;
            if (b > 0 && static_cast<int>(b) <= low_bin && level > low_peak) {
                low_peak = level;
            }
            if (static_cast<int>(b) >= high_bin && level > high_peak) {
                high_peak = level;
            }
            if (b > 0) {
                const double freq = static_cast<double>(b) * static_cast<double>(sample_rate) /
                                    static_cast<double>(window_frames);
                mag_sum += magnitude;
                weighted += freq * magnitude;
                if (static_cast<int>(b) <= low_bin) {
                    low_mag += magnitude;
                    low_weighted += freq * magnitude;
                }
            }
        }
        flux[static_cast<std::size_t>(f)] = sum;
        low_flux_raw[static_cast<std::size_t>(f)] = low_sum;
        high_flux_raw[static_cast<std::size_t>(f)] = high_sum;
        const double band = low_peak + high_peak;
        analysis.low_share.push_back(band > 0.0 ? static_cast<float>(low_peak / band) : 0.5f);
        analysis.energy.push_back(static_cast<float>(energy));
        analysis.centroid_hz.push_back(mag_sum > 0.0 ? static_cast<float>(weighted / mag_sum)
                                                     : 0.f);
        analysis.low_centroid_hz.push_back(
            low_mag > 0.0 ? static_cast<float>(low_weighted / low_mag) : 0.f);
        if (progress != nullptr && (f % 48 == 0 || f + 1 == count) &&
            !progress(user, f + 1, count)) {
            analysis.canceled = true;
            return analysis;
        }
    }
    // Remove the local mean so sustained loud passages do not read as onsets.
    const auto store_flux = [](const std::vector<double>& raw, std::vector<float>& dest) {
        dest.resize(raw.size());
        double running = 0.0;
        std::size_t lo = 0;
        std::size_t hi = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const std::size_t want_lo = i >= kAverageFrames / 2 ? i - kAverageFrames / 2 : 0;
            const std::size_t want_hi = std::min(raw.size(), i + kAverageFrames / 2);
            while (hi < want_hi) {
                running += raw[hi++];
            }
            while (lo < want_lo) {
                running -= raw[lo++];
            }
            const double mean = running / static_cast<double>(hi - lo);
            dest[i] = static_cast<float>(std::max(0.0, raw[i] - mean));
        }
    };
    store_flux(flux, analysis.envelope);
    store_flux(low_flux_raw, analysis.low_flux);
    store_flux(high_flux_raw, analysis.high_flux);
    analysis.silent = true;
    for (const float value : analysis.envelope) {
        if (value > 0.0f) {
            analysis.silent = false;
            break;
        }
    }
    return analysis;
}

std::vector<float> onset_envelope(std::span<const std::int16_t> interleaved, int frames) {
    OnsetAnalysis analysis = analyze_onsets(interleaved, frames);
    if (analysis.silent) {
        analysis.envelope.clear();
    }
    return std::move(analysis.envelope);
}

std::vector<std::int32_t> track_beats(std::span<const float> envelope,
                                      std::span<const float> period_ms,
                                      int hop_frames,
                                      int sample_rate) {
    std::vector<std::int32_t> beats;
    const std::size_t n = envelope.size();
    if (n == 0 || period_ms.size() != n) {
        return beats;
    }
    if (hop_frames < 1) {
        hop_frames = kOnsetHopFrames;
    }
    if (sample_rate < 1000) {
        sample_rate = kCddaRate;
    }
    double mean = 0.0;
    for (const float value : envelope) {
        mean += value;
    }
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (const float value : envelope) {
        var += (value - mean) * (value - mean);
    }
    const double sd = std::sqrt(var / static_cast<double>(n));
    if (!(sd > 0.0)) {
        return beats;
    }

    const double min_frames = frames_from_ms(kMinBeatMs, hop_frames, sample_rate);
    const double max_frames = frames_from_ms(kMaxBeatMs, hop_frames, sample_rate);
    std::vector<double> score(n);
    std::vector<std::ptrdiff_t> back(n, -1);
    for (std::size_t t = 0; t < n; ++t) {
        const double strength = static_cast<double>(envelope[t]) / sd;
        double p = frames_from_ms(period_ms[t], hop_frames, sample_rate);
        p = std::clamp(p, min_frames, max_frames);
        const double far = static_cast<double>(t) - 2.0 * p;
        const double near = static_cast<double>(t) - std::max(p / 2.0, min_frames);
        score[t] = strength;
        if (near < 0.0) {
            continue;
        }
        const std::size_t lo = far < 0.0 ? 0 : static_cast<std::size_t>(std::ceil(far));
        const std::size_t hi_limit = static_cast<std::size_t>(std::floor(near));
        const std::size_t hi = std::min(hi_limit, t);
        double best = -1e300;
        std::ptrdiff_t arg = -1;
        for (std::size_t tau = lo; tau <= hi; ++tau) {
            const double ratio = static_cast<double>(t - tau) / p;
            const double deviation = std::log(ratio);
            const double v = score[tau] - kPeriodPenalty * deviation * deviation;
            if (v > best) {
                best = v;
                arg = static_cast<std::ptrdiff_t>(tau);
            }
        }
        if (arg >= 0) {
            score[t] = strength + best;
            back[t] = arg;
        }
    }
    // End on the best-scoring frame within the last two periods.
    const double tail_p = std::clamp(
        frames_from_ms(period_ms[n - 1], hop_frames, sample_rate), min_frames, max_frames);
    const std::size_t tail = std::min(n, static_cast<std::size_t>(2.0 * tail_p) + 1u);
    std::size_t end = n - tail;
    for (std::size_t t = n - tail; t < n; ++t) {
        if (score[t] > score[end]) {
            end = t;
        }
    }
    for (std::ptrdiff_t t = static_cast<std::ptrdiff_t>(end); t >= 0;
         t = back[static_cast<std::size_t>(t)]) {
        beats.push_back(ms_from_frame(static_cast<std::size_t>(t), hop_frames, sample_rate));
    }
    std::reverse(beats.begin(), beats.end());
    return beats;
}

} // namespace oscilline
