// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Onset envelope and beat tracker when attack data is absent.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// Onset envelope frames are this many milliseconds apart (441 frames at 44.1 kHz).
inline constexpr int kOnsetStepMs = 10;
inline constexpr int kOnsetHopFrames = 441;
inline constexpr int kOnsetWindowFrames = 1024;

// Bins below this frequency are the low band. Bins above the high edge are the high band.
inline constexpr int kOnsetLowHz = 200;
inline constexpr int kOnsetHighHz = 2000;

// Per-frame view of the same window the onset envelope uses.
struct OnsetAnalysis {
    // Samples advanced between frames, and the rate those samples were taken at.
    // Frame i is at i * hop_frames * 1000 / sample_rate milliseconds. The disc
    // path keeps the 441-sample hop, which is exactly kOnsetStepMs.
    int hop_frames = kOnsetHopFrames;
    int sample_rate = 44100;
    // Non-negative spectral flux, one value per hop. Kept even when every
    // value is zero, so a smooth tone still has a frame for each hop.
    std::vector<float> envelope;
    // Peak low-band level over (peak low + peak high), in [0, 1]. Peaks, not sums,
    // so one strong bin is enough and the wider high band does not drown a low hit.
    // 0.5 when that frame is silent.
    std::vector<float> low_share;
    // Sum of log magnitudes. Silence is near 0.
    std::vector<float> energy;
    // Spectral centroid in Hz. 0 when that frame is silent.
    std::vector<float> centroid_hz;
    // Spectral flux of the low band and the high band, with the local mean removed.
    // Same length as `envelope`.
    std::vector<float> low_flux;
    std::vector<float> high_flux;
    // Centroid of the low band only, in Hz. 0 when that frame has no low-band energy.
    std::vector<float> low_centroid_hz;
    // True when no hop had a positive flux. onset_envelope drops the envelope then.
    bool silent = true;
    // True when `progress` asked to stop.
    bool canceled = false;
};

// Called with the number of hops finished. Return false to stop.
using OnsetProgress = bool (*)(void* user, int hop, int hop_count);

// Spectral flux plus low/high energy of interleaved stereo. `sample_rate`,
// `window_frames`, and `hop_frames` default to CD audio and a 10 ms hop.
// Empty when the audio is shorter than one window. `window_frames` is a power of two.
[[nodiscard]] OnsetAnalysis analyze_onsets(std::span<const std::int16_t> interleaved,
                                           int frames,
                                           OnsetProgress progress = nullptr,
                                           void* user = nullptr,
                                           int sample_rate = 44100,
                                           int window_frames = kOnsetWindowFrames,
                                           int hop_frames = kOnsetHopFrames);

// Spectral-flux onset strength, one value per kOnsetStepMs. Values are
// non-negative. Returns an empty vector for silence or for audio shorter than
// one window.
[[nodiscard]] std::vector<float> onset_envelope(std::span<const std::int16_t> interleaved,
                                                int frames);

// Picks beats from an onset envelope by dynamic programming. `period_ms` gives
// the expected beat period for each envelope frame (same length as `envelope`).
// Beats are ascending times in milliseconds and are at least kMinBeatMs apart.
// `hop_frames` and `sample_rate` match the envelope's analysis. Defaults match
// the 10 ms disc hop. Returns an empty vector when the envelope carries no onsets.
[[nodiscard]] std::vector<std::int32_t> track_beats(std::span<const float> envelope,
                                                    std::span<const float> period_ms,
                                                    int hop_frames = kOnsetHopFrames,
                                                    int sample_rate = 44100);

} // namespace oscilline
