// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Charts a user WAV, FLAC, or MP3 with no disc.

#pragma once

#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/mapping.hpp"
#include "oscilline/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// A custom track shorter than this is refused.
inline constexpr int kMinMusicMs = 20000;
// Longer tracks are cut here. Playback stays inside the same cap as CD audio.
inline constexpr int kMaxMusicSeconds = 15 * 60;
// Used when the onset envelope has no tempo peak.
inline constexpr int kDefaultMusicBeatMs = 500;

// Obstacles per second. Bronze is easy, silver medium, gold hard. A dense
// track aims just above its band minimum, not at the middle. The rate does not
// depend on a hash of the samples, so a re-encode stays in the same band.
inline constexpr float kBronzeDensityMin = 0.40f;
inline constexpr float kBronzeDensityMax = 0.80f;
inline constexpr float kSilverDensityMin = 0.80f;
inline constexpr float kSilverDensityMax = 1.50f;
inline constexpr float kGoldDensityMin = 1.50f;
inline constexpr float kGoldDensityMax = 2.20f;
// Beats an obstacle stays on screen. Easy is longer; hard is shorter.
inline constexpr int kBronzeApproachBeats = kApproachBeats;
inline constexpr int kSilverApproachBeats = 3;
inline constexpr int kGoldApproachBeats = 2;
// The chart aims at this fraction of the onset rate, counting every peak, then
// clamps between the rising floor and kDensityAimAboveMin over the band minimum.
// The fill takes strong peaks, then weak peaks by strength, then the beat grid.
inline constexpr float kOnsetDensityScale = 0.75f;
// How far above the band minimum a dense track sits. Bronze aims at 0.50/s.
inline constexpr float kDensityAimAboveMin = 0.10f;
// At a low onset rate the lower clamp is this fraction of the band minimum.
// It rises to the minimum itself across the knee below, so a re-encode that
// adds or drops a few peaks cannot jump the chart from the floor to the band.
inline constexpr float kSparseDensityFloor = 0.80f;
inline constexpr float kDensityKneeLow = 2.f;
inline constexpr float kDensityKneeHigh = 3.f;
// Hit picking is local to windows of this length, anchored at t = 0, so one
// dropped onset cannot shift every later hit.
inline constexpr int kSelectWindowMs = 2000;

// Pairs are mixed through the whole chart, not saved for the loudest hits.
// A typical track sits near the aim. Energy and onset strength may move it,
// and the result is clamped to the min/max band.
inline constexpr float kPairFractionAim = 0.30f;
inline constexpr float kPairFractionMin = 0.20f;
inline constexpr float kPairFractionMax = 0.45f;
// How far activity (0..1, 0.5 typical) moves the share off the aim. Busy
// commercial tracks sit near 0.9, so this keeps them close to 0.30.
inline constexpr float kPairActivitySwing = 0.10f;
inline constexpr float kBronzePairFraction = kPairFractionAim;
inline constexpr float kSilverPairFraction = kPairFractionAim;
inline constexpr float kGoldPairFraction = kPairFractionAim;

// Charting decimates CD audio by this factor before the FFT. The hop stays
// near 10 ms, so a peak time is the transient itself and not a grid cell.
inline constexpr int kMusicDecimate = 4;
inline constexpr int kMusicWindowFrames = 256;
inline constexpr int kMusicHopFrames = 110;
// A beat and an onset peak this close are one hit, kept at the peak time.
inline constexpr int kOnsetMergeMs = 30;
// Loudest spectral flux below this is a tone. It is charted as an even loop grid
// instead of a string of numerical onsets.
inline constexpr float kTonalPeakFlux = 8.f;
// Tempo is read from the decimated low-band flux and snapped to this many BPM.
// A neighbor within kTempoHysteresis of the best score yields to the tempo
// nearer kTempoReferenceBpm, so a re-encode keeps the approach within about 2%.
inline constexpr float kTempoHysteresis = 0.06f;
inline constexpr int kTempoReferenceBpm = 120;

// Clip playback relative to kTempoReferenceBpm. Scroll time is four beats
// clamped to the approach range, so this is that time at 120 BPM divided by
// the same time at `beat_period_ms`. The rate at the reference tempo is 1.
[[nodiscard]] inline float rabbit_tempo_scale(int beat_period_ms) {
    if (beat_period_ms <= 0) {
        return 1.f;
    }
    const auto approach = [](double beat_ms) {
        const double ms = beat_ms * static_cast<double>(kApproachBeats);
        if (ms < static_cast<double>(kMinApproachMs)) {
            return static_cast<double>(kMinApproachMs);
        }
        if (ms > static_cast<double>(kMaxApproachMs)) {
            return static_cast<double>(kMaxApproachMs);
        }
        return ms;
    };
    const double reference = approach(60000.0 / static_cast<double>(kTempoReferenceBpm));
    return static_cast<float>(reference / approach(static_cast<double>(beat_period_ms)));
}
inline constexpr int kTempoBpmQuantum = 2;
// A tempo and its double that both score within kTempoDoubleFit of the best
// take the faster one, up to kTempoFastMaxBpm. So 170 BPM clicks lock to 170.
inline constexpr float kTempoDoubleFit = 0.88f;
inline constexpr int kTempoFastMaxBpm = 180;
// Sequence model. Runs of one base type are this long. Audio may nudge a hit
// with this chance; the seeded sequence still decides the rest.
inline constexpr int kTypeRunMin = 1;
inline constexpr int kTypeRunMax = 4;
inline constexpr float kTypeNudgeChance = 0.12f;
// A repeated section copies this many obstacles from the earlier phrase.
inline constexpr int kPhraseObstaclesMin = 4;
inline constexpr int kPhraseObstaclesMax = 8;
inline constexpr int kPhraseWindowMs = 12000;
inline constexpr float kPhraseSimilarity = 0.90f;

// Placement, from black-box custom-music play. The loudness gate, the noise bed
// and long silences are on by default after a per-track comparison with the
// original. Track density and the beat stride stay behind `--gen-experimental`:
// they match the original's density and gaps but move hits when a re-encode
// changes the onset count. Each is its own constant, so a change is one line.
inline constexpr bool kGenTrackDensity = false;
inline constexpr float kExperimentalDensityMin = 0.10f;
inline constexpr float kExperimentalDensityMax = 0.45f;
// Onsets per second where the track density leaves the minimum and reaches the maximum.
inline constexpr float kExperimentalDensityKneeLow = 0.50f;
inline constexpr float kExperimentalDensityKneeHigh = 2.50f;
inline constexpr bool kGenBeatStride = false;
inline constexpr int kExperimentalMinGapMs = 2000;
inline constexpr int kExperimentalStrideMinBeats = 4;
inline constexpr int kExperimentalStrideMaxBeats = 7;
inline constexpr bool kGenLoudnessGate = true;
// Short-term peak level, not RMS: the window peak opens the gate and holds it.
inline constexpr float kLoudnessOpenDbfs = -20.f;
inline constexpr float kLoudnessCloseDbfs = -24.f;
inline constexpr int kLoudnessWindowMs = 300;
inline constexpr bool kGenSteadyNoise = true;
// About eight obstacles on a ~70 s noise bed, where the default path charts ~35.
inline constexpr float kExperimentalNoiseDensity = 0.10f;
// Steady noise has no onsets standing out: its 99th-percentile flux stays under
// this multiple of the 90th. The disc tracks measure about 3.8–7.6.
inline constexpr float kNoiseOnsetContrast = 2.5f;
inline constexpr bool kGenLongSilence = true;
inline constexpr int kLongSilenceMs = 8000;

struct MusicLimits {
    int min_ms = kMinMusicMs;
    int max_seconds = kMaxMusicSeconds;
};

struct DecodedMusic {
    // 44.1 kHz interleaved stereo, the same layout as CD audio.
    std::vector<std::int16_t> interleaved;
    int frames = 0;
    // True when the file was longer than max_seconds and the tail was dropped.
    bool truncated = false;
    // 32 hex digits over the little-endian decoded samples. High scores use this.
    // The sequence model hashes the same samples for its seed. Hit times follow
    // the onsets, so a re-encode keeps the times and can still change types.
    std::string fingerprint;
    std::uint32_t seed = 0;
};

// Return false to stop a decode or a chart. `fraction` is in [0, 1].
using MusicProgress = bool (*)(void* user, float fraction, const char* stage);

struct MusicChart {
    CourseTimeline timeline;
    std::vector<float> envelope;
};

struct DensityBand {
    float min = 0.f;
    float max = 0.f;
};

// Bronze is the easy band, silver medium, gold hard.
[[nodiscard]] DensityBand density_band(Difficulty difficulty);

// The middle of that difficulty's band.
[[nodiscard]] float density_target(Difficulty difficulty);

// Obstacles per second for an onset rate. This is
// clamp(kOnsetDensityScale * onsets_per_second,
//       lerp(kSparseDensityFloor, 1, smoothstep(rate, knee)) * band.min,
//       band.min + kDensityAimAboveMin).
[[nodiscard]] float onset_limited_density(float onsets_per_second, Difficulty difficulty);

// Nominal pair share. The chart may sit anywhere in kPairFractionMin..Max.
// `difficulty` does not change it.
[[nodiscard]] float pair_fraction(Difficulty difficulty);

// How many obstacles `density_target` asks for across `duration_ms`.
[[nodiscard]] int music_target_count(int duration_ms, Difficulty difficulty);

// Decodes WAV, FLAC, or MP3 bytes. `name` is only used to read the extension.
[[nodiscard]] Result<DecodedMusic> decode_music_bytes(std::span<const std::uint8_t> bytes,
                                                      std::string_view name,
                                                      MusicLimits limits = {},
                                                      MusicProgress progress = nullptr,
                                                      void* progress_user = nullptr);

[[nodiscard]] Result<DecodedMusic> decode_music_file(const std::filesystem::path& file,
                                                     MusicLimits limits = {},
                                                     MusicProgress progress = nullptr,
                                                     void* progress_user = nullptr);

// `experimental` turns on every `kGen*` behavior whose constant is still false.
// Bronze, silver, and gold still change density, approach, and the type seed.
struct MusicChartOptions {
    bool experimental = false;
};

// Disc-camera row for a custom chart: bronze is course 1, silver course 3,
// gold course 5. Playback still needs the section spans from
// custom_music_camera_sections.
[[nodiscard]] inline int custom_music_camera_course(Difficulty difficulty) {
    return courses_for(difficulty).first;
}

// Gold custom music uses the same screen shift as disc courses 5 and 6.
[[nodiscard]] inline bool custom_music_gold_shift(Difficulty difficulty) {
    return difficulty == Difficulty::Gold;
}

// Quarter-beat grid the custom chart aligns perfect centers to: each tracked
// beat plus the half- and quarter-beat points the generator already offers.
// With no tracked beats the grid is `period_ms / 4` from time 0 through
// `end_ms` plus one period.
[[nodiscard]] std::vector<std::int32_t>
music_beat_grid(std::span<const std::int32_t> beats, int period_ms, std::int32_t end_ms);

// The chart hit plus the late bias moves onto a grid point only when that
// point is this close to both the hit and that mark. The hit's own beat is
// the one that defines it; a neighboring quarter is left alone, and so is a
// swung onset. The perfect window is not this mark.
inline constexpr int kPerfectBeatSnapMs = 40;

// Shifts each hit so the chart time plus kJudgmentLateBiasMs lands on that
// grid when the point is within kPerfectBeatSnapMs of the hit and of that
// mark. The same rule applies to every obstacle, including a loop whose front
// edge is left of the hit. Otherwise the hit stays on the chart. Keeps
// kMinGapMs. Disc courses do not call this. An attack hit stays on the
// detector's stamp, and the snap mark is that stamp plus the late bias.
void align_perfect_centers(std::vector<CourseEvent>& events,
                           std::span<const std::int32_t> beats,
                           int period_ms);

// Builds a course from decoded 44.1 kHz stereo. Hit times are onset peaks,
// with beats only as a fill, inside fixed time windows, then a hit whose time
// plus the late bias is near its own beat is moved so that mark lands on the
// beat grid above. Obstacle types
// and pairs come from a seeded sequence, with a short copied phrase when a
// section repeats.
// The same samples and difficulty always chart the same course. Bronze is easy,
// silver medium, gold hard: fewer and slower obstacles on bronze, more and
// faster on gold. The type seed uses that tier's FSL course number (1, 3, or
// 5), the same multiplier as a disc course.
[[nodiscard]] Result<MusicChart> chart_music(std::span<const std::int16_t> interleaved,
                                             int frames,
                                             Difficulty difficulty,
                                             MusicProgress progress = nullptr,
                                             void* progress_user = nullptr,
                                             MusicChartOptions options = {});

struct MusicScore {
    std::string fingerprint;
    Difficulty difficulty = Difficulty::Bronze;
    int score = 0;
};

struct MusicScoreBook {
    std::vector<MusicScore> rows;
};

[[nodiscard]] int
music_best(const MusicScoreBook& book, std::string_view fingerprint, Difficulty difficulty);

// Keeps the higher score for that fingerprint and difficulty.
void music_note(MusicScoreBook& book,
                std::string_view fingerprint,
                Difficulty difficulty,
                int score);

[[nodiscard]] Result<MusicScoreBook> load_music_scores(const std::filesystem::path& file);

// Writes the book, creating parent directories. An empty path is a no-op success.
[[nodiscard]] Result<int> save_music_scores(const std::filesystem::path& file,
                                            const MusicScoreBook& book);

// Directory that holds music-scores.txt and settings.txt.
// Empty when no home directory can be found.
[[nodiscard]] std::filesystem::path user_config_directory();

// User config file. Empty when no home directory can be found.
[[nodiscard]] std::filesystem::path music_score_path();

} // namespace oscilline
