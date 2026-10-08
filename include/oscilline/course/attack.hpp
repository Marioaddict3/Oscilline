// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Live audio attack detection for the built-in courses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// Live attack analysis for the built-in courses. The original charts each disc
// course from the CD audio while it plays; the script only gives speed, shadow,
// and pattern. See docs/course-mapping.md.

// All constants of the live attack analysis, in one place. Every value comes
// from black-box RAM observation of the original on an emulator (the analysis
// state, its chunk ring and the hit flag, read as data while courses 1, 3 and 5
// played), never from its code. Only derived numbers are kept; the logs are not
// in this repository. See docs/course-mapping.md for the match against them.
//
// Emphasis. Ps is the mean square of the full-rate mono mix (L + R) / 2 over
// the last kAttackShortFrames frames; it matches the logged short energy to
// 0.1%. Pl is the same over kAttackLongFrames frames; the logged long energy
// is 1-2.6% off this box, its exact definition is not known.
inline constexpr int kAttackShortFrames = 1024;
inline constexpr int kAttackLongFrames = 8192;
// Frames between emphasis evaluations. The game hops by half a 256-frame
// capture chunk (128 clock units).
inline constexpr int kAttackHopFrames = 128;
// An evaluation is stamped this many frames after the end of its windows.
// Fitted against the observed attack times of courses 1, 3 and 5.
inline constexpr int kAttackStampFrames = 120;
// Selection. Select windows are laid back to back from 0, each as long as the
// script shadow value (FSL field 2), and offer their largest emphasis as a
// potential attack. A pending attack commits at the first window end at least
// kAttackCommitSpeeds speed periods after it. Before that, a potential more
// than kAttackReplaceRatio times its emphasis replaces it (every logged
// replacement had a ratio of at least 1.52). After a commit, the first
// potential at least kAttackCommitSpeeds speed periods later becomes pending,
// whatever its emphasis. Each obstacle collides at its audible attack.
inline constexpr double kAttackCommitSpeeds = 2.0;
inline constexpr double kAttackReplaceRatio = 1.5;
// Mean square of the mono mix. 32.768^2 is −60 dBFS against full scale 32768.
// Below this the long window is silence, not an attack.
inline constexpr double kAttackSilenceMeanSquare = 32.768 * 32.768;

// Emphasis Ps / Pl for each hop, from the mono mix (L + R) / 2 of interleaved
// stereo. Evaluation k ends at frame (k + 1) * kAttackHopFrames. Ps is the power
// of the last kAttackShortFrames frames and Pl of the last kAttackLongFrames
// frames, so the value lies in 0..1. An evaluation that ends before the long
// window is full, has no power in it, or sits under `kAttackSilenceMeanSquare`,
// is 0 and offers no attack. Course 2
// (and course 4) open loud: the original's first attack there is the one after
// the long window fills, not the louder ratio over a part-filled window.
[[nodiscard]] std::vector<float> attack_emphasis(std::span<const std::int16_t> interleaved,
                                                 int frames);

// Time stamp of evaluation `k` in frames on the 44100 Hz CD clock.
[[nodiscard]] std::int64_t attack_stamp_frame(std::size_t k);

// Time stamp of evaluation `k` in milliseconds on the 44100 Hz CD clock.
[[nodiscard]] double attack_stamp_ms(std::size_t k);

// Milliseconds of audio that are still above the silence floor. 0 when the
// buffer is silent or empty. A file that stays loud through its last hop
// returns the full duration, so a loud ending is not trimmed.
[[nodiscard]] std::int32_t attack_audible_end_ms(std::span<const std::int16_t> interleaved,
                                                 int frames);

} // namespace oscilline
