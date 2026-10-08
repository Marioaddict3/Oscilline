// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Custom-music chart, duration limits, and the score file.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/music.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/course/shapes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::uint8_t> wav_bytes(int rate, const std::vector<std::int16_t>& stereo) {
    const auto data_bytes = static_cast<std::uint32_t>(stereo.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> out(44 + data_bytes);
    const auto put = [&](std::size_t at, const char* text) {
        for (int i = 0; i < 4; ++i) {
            out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(text[i]);
        }
    };
    const auto u16 = [&](std::size_t at, std::uint16_t value) {
        out[at] = static_cast<std::uint8_t>(value & 0xffu);
        out[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto u32 = [&](std::size_t at, std::uint32_t value) {
        u16(at, static_cast<std::uint16_t>(value));
        u16(at + 2, static_cast<std::uint16_t>(value >> 16));
    };
    put(0, "RIFF");
    u32(4, 36 + data_bytes);
    put(8, "WAVE");
    put(12, "fmt ");
    u32(16, 16);
    u16(20, 1);
    u16(22, 2);
    u32(24, static_cast<std::uint32_t>(rate));
    u32(28, static_cast<std::uint32_t>(rate) * 4u);
    u16(32, 4);
    u16(34, 16);
    put(36, "data");
    u32(40, data_bytes);
    for (std::size_t i = 0; i < stereo.size(); ++i) {
        const auto sample = static_cast<std::uint16_t>(stereo[i]);
        out[44 + i * 2] = static_cast<std::uint8_t>(sample & 0xffu);
        out[45 + i * 2] = static_cast<std::uint8_t>(sample >> 8);
    }
    return out;
}

std::vector<std::int16_t>
clicks_every(int rate, int seconds, int hz, int period, double amp = 20000.0) {
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    if (period < 1) {
        period = 1;
    }
    for (int i = 0; i < rate * seconds; ++i) {
        const int tick = i % period;
        std::int16_t sample = 0;
        constexpr int kBurst = 1024;
        if (tick < kBurst) {
            const double phase = 3.141592653589793 * tick / (kBurst - 1);
            const double env = std::sin(phase);
            const double tone = std::sin(2.0 * 3.141592653589793 * hz * i / rate);
            sample = static_cast<std::int16_t>(
                std::lround(std::clamp(tone * env * amp, -32767.0, 32767.0)));
        }
        stereo[static_cast<std::size_t>(i) * 2u] = sample;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }
    return stereo;
}

std::vector<std::int16_t> clicks(int rate, int seconds, int hz) {
    return clicks_every(rate, seconds, hz, rate / 2);
}

std::vector<std::int16_t> tone(int rate, int seconds, int hz) {
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        const double wave = std::sin(2.0 * 3.141592653589793 * hz * i / rate);
        const auto sample = static_cast<std::int16_t>(wave * 12000.0);
        stereo[static_cast<std::size_t>(i) * 2u] = sample;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }
    return stereo;
}

} // namespace

int obstacle_kind(std::uint8_t id);

// The band a whole chart lands in. With track density on, the base band is
// 0.10–0.45 per second; otherwise that difficulty's own band.
oscilline::DensityBand chart_band(oscilline::Difficulty difficulty) {
    if (oscilline::kGenTrackDensity) {
        return {oscilline::kExperimentalDensityMin, oscilline::kExperimentalDensityMax};
    }
    return oscilline::density_band(difficulty);
}

TEST_CASE("rabbit playback matches the scroll tempo factor") {
    using namespace oscilline;
    CHECK(rabbit_tempo_scale(0) == doctest::Approx(1.f));
    CHECK(rabbit_tempo_scale(60000 / kTempoReferenceBpm) == doctest::Approx(1.f));
    // Four beats at 240 BPM is the one-second floor, twice the 120 BPM crossing.
    CHECK(rabbit_tempo_scale(250) == doctest::Approx(2.f));
    // Four beats at 60 BPM clamps to the three-second ceiling.
    CHECK(rabbit_tempo_scale(1000) == doctest::Approx(2000.0 / 3000.0));
    CHECK(rabbit_tempo_scale(100) == doctest::Approx(2.f));
    CHECK(rabbit_tempo_scale(4000) == doctest::Approx(2000.0 / 3000.0));
}

TEST_CASE("a short or corrupt music file is refused") {
    using namespace oscilline;
    const std::vector<std::uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8};
    const auto bad_wav = decode_music_bytes(junk, "noise.wav");
    CHECK_FALSE(bad_wav);
    CHECK(bad_wav.error().find("could not read") != std::string::npos);
    const auto bad_mp3 = decode_music_bytes(junk, "noise.mp3");
    CHECK_FALSE(bad_mp3);
    const auto bad_flac = decode_music_bytes(junk, "noise.flac");
    CHECK_FALSE(bad_flac);
    const auto odd = decode_music_bytes(junk, "noise.ogg");
    CHECK_FALSE(odd);
    CHECK(odd.error().find("not wav") != std::string::npos);

    const auto short_wav = wav_bytes(kCddaRate, clicks(kCddaRate, 3, 100));
    const auto too_short = decode_music_bytes(short_wav, "short.wav");
    CHECK_FALSE(too_short);
    CHECK(too_short.error().find("20") != std::string::npos);

    const auto missing = decode_music_file("oscilline-no-such-track.wav");
    CHECK_FALSE(missing);
    CHECK(missing.error().find("missing") != std::string::npos);

    auto lying = wav_bytes(kCddaRate, clicks(kCddaRate, 1, 100));
    const auto claim = static_cast<std::uint32_t>(30 * kCddaRate * 4);
    lying[4] = static_cast<std::uint8_t>((36 + claim) & 0xffu);
    lying[5] = static_cast<std::uint8_t>(((36 + claim) >> 8) & 0xffu);
    lying[6] = static_cast<std::uint8_t>(((36 + claim) >> 16) & 0xffu);
    lying[7] = static_cast<std::uint8_t>(((36 + claim) >> 24) & 0xffu);
    lying[40] = static_cast<std::uint8_t>(claim & 0xffu);
    lying[41] = static_cast<std::uint8_t>((claim >> 8) & 0xffu);
    lying[42] = static_cast<std::uint8_t>((claim >> 16) & 0xffu);
    lying[43] = static_cast<std::uint8_t>((claim >> 24) & 0xffu);
    const auto damaged = decode_music_bytes(lying, "cut.wav");
    CHECK_FALSE(damaged);
    CHECK(damaged.error().find("20") == std::string::npos);
    CHECK(damaged.error().find("missing") == std::string::npos);
    CHECK(damaged.error().find("damaged") != std::string::npos);

    auto long_cut = wav_bytes(kCddaRate, clicks(kCddaRate, 22, 100));
    const auto long_claim = static_cast<std::uint32_t>(60 * kCddaRate * 4);
    const auto put_u32 = [](std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
        bytes[at] = static_cast<std::uint8_t>(value & 0xffu);
        bytes[at + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
        bytes[at + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
        bytes[at + 3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
    };
    put_u32(long_cut, 4, 36 + long_claim);
    put_u32(long_cut, 40, long_claim);
    const auto long_damaged = decode_music_bytes(long_cut, "long-cut.wav");
    CHECK_FALSE(long_damaged);
    CHECK(long_damaged.error().find("damaged") != std::string::npos);
    CHECK(long_damaged.error().find("20") == std::string::npos);
}

TEST_CASE("the same music file and difficulty always chart the same course") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto pcm = clicks(kCddaRate, kSeconds, 100);
    const auto bytes = wav_bytes(kCddaRate, pcm);
    const auto decoded = decode_music_bytes(bytes, "clicks.wav");
    REQUIRE(decoded);
    CHECK(decoded.value().frames > kCddaRate * 21);
    CHECK(decoded.value().fingerprint.size() == 32);
    CHECK_FALSE(decoded.value().truncated);

    const auto again = decode_music_bytes(bytes, "clicks.wav");
    REQUIRE(again);
    CHECK(again.value().fingerprint == decoded.value().fingerprint);
    CHECK(again.value().seed == decoded.value().seed);

    const auto first =
        chart_music(decoded.value().interleaved, decoded.value().frames, Difficulty::Bronze);
    const auto second =
        chart_music(decoded.value().interleaved, decoded.value().frames, Difficulty::Bronze);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().timeline.events.size() == second.value().timeline.events.size());
    for (std::size_t i = 0; i < first.value().timeline.events.size(); ++i) {
        CHECK(first.value().timeline.events[i].hit_ms == second.value().timeline.events[i].hit_ms);
        CHECK(first.value().timeline.events[i].obstacle ==
              second.value().timeline.events[i].obstacle);
    }

    const int duration = cdda_duration_ms(decoded.value().frames);
    const double seconds = static_cast<double>(duration) / 1000.0;
    const auto check_band = [&](Difficulty difficulty) {
        const auto chart =
            chart_music(decoded.value().interleaved, decoded.value().frames, difficulty);
        REQUIRE(chart);
        const auto& events = chart.value().timeline.events;
        REQUIRE(!events.empty());
        const double rate = static_cast<double>(events.size()) / seconds;
        const DensityBand band = chart_band(difficulty);
        // A 2 Hz pulse measures a little under 2 onsets per second after the
        // lead-in, so 0.75 of that rate can sit just under the gold floor.
        CHECK(rate + 0.05 >= static_cast<double>(band.min));
        CHECK(rate <= static_cast<double>(band.max));
        CHECK(chart.value().timeline.audio_end_ms == duration);
        CHECK(chart.value().timeline.duration_ms >= duration);
        for (const auto& event : events) {
            CHECK(event.hit_ms <= chart.value().timeline.audio_end_ms);
            CHECK_FALSE(obstacle_after_audio(
                event.hit_ms, event.scroll_approach_ms, chart.value().timeline.audio_end_ms));
        }
        for (std::size_t i = 1; i < events.size(); ++i) {
            CHECK(events[i].hit_ms - events[i - 1].hit_ms >= kMinGapMs);
            const std::int64_t previous =
                static_cast<std::int64_t>(events[i - 1].hit_ms) - events[i - 1].scroll_approach_ms;
            const std::int64_t entry =
                static_cast<std::int64_t>(events[i].hit_ms) - events[i].scroll_approach_ms;
            CHECK(entry >= previous);
        }
    };
    check_band(Difficulty::Bronze);
    check_band(Difficulty::Silver);
    check_band(Difficulty::Gold);

    const auto gold =
        chart_music(decoded.value().interleaved, decoded.value().frames, Difficulty::Gold);
    REQUIRE(gold);
    int pairs = 0;
    for (const auto& event : gold.value().timeline.events) {
        if (event.obstacle >= 4) {
            ++pairs;
        }
    }
    const double share =
        static_cast<double>(pairs) / static_cast<double>(gold.value().timeline.events.size());
    CHECK(share >= static_cast<double>(kPairFractionMin));
    CHECK(share <= static_cast<double>(kPairFractionMax));
}

TEST_CASE("pitch does not choose the obstacle type") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto low =
        chart_music(clicks(kCddaRate, kSeconds, 80), kCddaRate * kSeconds, Difficulty::Bronze);
    const auto high =
        chart_music(clicks(kCddaRate, kSeconds, 4000), kCddaRate * kSeconds, Difficulty::Bronze);
    const auto pad =
        chart_music(tone(kCddaRate, kSeconds, 110), kCddaRate * kSeconds, Difficulty::Bronze);
    REQUIRE(low);
    REQUIRE(high);
    REQUIRE(pad);
    const auto kinds_used = [](const MusicChart& chart) {
        bool seen[4] = {};
        int count = 0;
        for (const auto& event : chart.timeline.events) {
            seen[obstacle_kind(event.obstacle)] = true;
        }
        for (const bool kind : seen) {
            count += kind ? 1 : 0;
        }
        return count;
    };
    CHECK(kinds_used(low.value()) >= 3);
    CHECK(kinds_used(high.value()) >= 3);
    // A held tone charts sparsely (about 0.10 per second), too few hits to need
    // three kinds. A longer pad still mixes them.
    if (pad.value().timeline.events.size() >= 8) {
        CHECK(kinds_used(pad.value()) >= 3);
    }
}

TEST_CASE("music scores keep the best total for a fingerprint") {
    using namespace oscilline;
    const auto dir = std::filesystem::temp_directory_path() / "oscilline-music-scores";
    std::filesystem::create_directories(dir);
    const auto file = dir / "scores.txt";
    std::filesystem::remove(file);

    MusicScoreBook book;
    music_note(book, "abc", Difficulty::Silver, 400);
    music_note(book, "abc", Difficulty::Silver, 150);
    CHECK(music_best(book, "abc", Difficulty::Silver) == 400);
    music_note(book, "abc", Difficulty::Silver, 900);
    music_note(book, "abc", Difficulty::Gold, 10);
    CHECK(music_best(book, "abc", Difficulty::Silver) == 900);
    CHECK(music_best(book, "abc", Difficulty::Gold) == 10);
    REQUIRE(save_music_scores(file, book));
    const auto loaded = load_music_scores(file);
    REQUIRE(loaded);
    CHECK(music_best(loaded.value(), "abc", Difficulty::Silver) == 900);
    CHECK(music_best(loaded.value(), "missing", Difficulty::Bronze) == 0);
    std::filesystem::remove(file);
}

TEST_CASE("silence has no chart") {
    using namespace oscilline;
    const std::vector<std::int16_t> quiet(static_cast<std::size_t>(kCddaRate * 22) * 2u, 0);
    const auto chart = chart_music(quiet, kCddaRate * 22, Difficulty::Bronze);
    CHECK_FALSE(chart);
    CHECK(chart.error().find("no rhythm") != std::string::npos);
}

int obstacle_kind(std::uint8_t id) {
    switch (id) {
    case 0:
    case 4:
    case 5:
    case 6:
        return 0;
    case 1:
    case 8:
        return 1;
    case 2:
    case 7:
        return 2;
    case 3:
    case 9:
        return 3;
    default:
        return 0;
    }
}

std::vector<std::int16_t> kit(int rate, int seconds, bool noise) {
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    std::uint32_t rng = 1;
    for (int i = 0; i < rate * seconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double cycle = std::fmod(t, 0.5);
        const auto burst = [&](double at, double hz, double amp, double dur) {
            const double dt = cycle - at;
            if (dt < 0.0 || dt >= dur) {
                return 0.0;
            }
            const double env = std::sin(3.141592653589793 * dt / dur);
            return std::sin(2.0 * 3.141592653589793 * hz * t) * env * amp;
        };
        double sample = std::sin(2.0 * 3.141592653589793 * 220.0 * t) * 0.12;
        sample += burst(0.0, 55.0, 0.95, 0.09);
        sample += burst(0.125, 8000.0, 0.55, 0.03);
        sample += burst(0.25, 180.0, 0.75, 0.07);
        sample += burst(0.25, 3200.0, 0.3, 0.04);
        sample += burst(0.375, 7000.0, 0.5, 0.03);
        int value = static_cast<int>(std::lround(sample * 20000.0));
        if (noise) {
            rng = rng * 1664525u + 1013904223u;
            value += static_cast<int>(rng >> 16) % 401 - 200;
        }
        value = std::clamp(value, -32768, 32767);
        const auto stored = static_cast<std::int16_t>(value);
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

void expect_similar_chart(const oscilline::MusicChart& left, const oscilline::MusicChart& right) {
    const auto& a = left.timeline.events;
    const auto& b = right.timeline.events;
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
    const int count_gap = std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size()));
    CHECK(count_gap <= std::max(2, static_cast<int>(a.size()) / 8));
    std::vector<char> used(b.size(), 0);
    int matched = 0;
    int same = 0;
    for (const auto& event : a) {
        int best = -1;
        int best_dt = 51;
        for (std::size_t i = 0; i < b.size(); ++i) {
            if (used[i] != 0) {
                continue;
            }
            const int dt = std::abs(event.hit_ms - b[i].hit_ms);
            if (dt <= 50 && dt < best_dt) {
                best_dt = dt;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) {
            continue;
        }
        used[static_cast<std::size_t>(best)] = 1;
        ++matched;
    }
    CHECK(matched * 5 >= static_cast<int>(a.size()) * 4);
    (void)same;
}

TEST_CASE("a re-encode of the same performance charts the same course") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto cd = wav_bytes(kCddaRate, kit(kCddaRate, kSeconds, false));
    const auto fast = wav_bytes(48000, kit(48000, kSeconds, true));
    const auto decoded_cd = decode_music_bytes(cd, "kit.wav");
    const auto decoded_fast = decode_music_bytes(fast, "kit48.wav");
    REQUIRE(decoded_cd);
    REQUIRE(decoded_fast);
    for (const Difficulty difficulty : {Difficulty::Bronze, Difficulty::Silver, Difficulty::Gold}) {
        const auto left =
            chart_music(decoded_cd.value().interleaved, decoded_cd.value().frames, difficulty);
        const auto right =
            chart_music(decoded_fast.value().interleaved, decoded_fast.value().frames, difficulty);
        REQUIRE(left);
        REQUIRE(right);
        expect_similar_chart(left.value(), right.value());
        REQUIRE(!left.value().timeline.events.empty());
        REQUIRE(!right.value().timeline.events.empty());
        const int left_approach = left.value().timeline.events.front().approach_ms;
        const int right_approach = right.value().timeline.events.front().approach_ms;
        const int approach_gap = std::abs(left_approach - right_approach);
        CHECK(approach_gap * 100 <= std::max(left_approach, 1) * 2);
    }
}

TEST_CASE("perfect centers snap onto the custom beat grid") {
    using namespace oscilline;
    const std::vector<std::int32_t> beats{1000, 1500, 2000, 2500, 3000, 3500};
    std::vector<CourseEvent> events;
    for (const int hit : {1040, 1610, 2488}) {
        CourseEvent event;
        event.obstacle = events.size() == 1 ? kLoopObstacleId : static_cast<std::uint8_t>(0);
        event.hit_ms = hit;
        event.approach_ms = 2000;
        event.scroll_approach_ms = 2000;
        event.beat_ms = 500;
        events.push_back(event);
    }
    const std::vector<std::int32_t> defined{1040, 1610, 2488};
    // The loop's perfect midpoint sits well before the hit. Snap still uses
    // the chart hit plus the late bias, so that lead does not keep it off the grid.
    const ObstacleWindow loop_before = obstacle_window(events[1]);
    const std::int64_t loop_mid = (loop_before.perfect_open + loop_before.perfect_close) / 2;
    CHECK(events[1].hit_ms - loop_mid > kPerfectBeatSnapMs);

    align_perfect_centers(events, beats, 500);
    const std::vector<std::int32_t> grid = music_beat_grid(beats, 500, 4000);
    std::int32_t previous = -100000;
    REQUIRE(events.size() == defined.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const CourseEvent& event = events[i];
        const std::int64_t mark = event.hit_ms + kJudgmentLateBiasMs;
        const bool on_grid =
            std::find(grid.begin(), grid.end(), static_cast<std::int32_t>(mark)) != grid.end();
        // 1040's mark (1060) sits outside the snap window of every grid point,
        // so the hit stays. The loop at 1610 and the block at 2488 are already
        // near the beat that defines them, and both move the same way.
        if (i == 0) {
            CHECK(event.hit_ms == defined[i]);
            CHECK(mark == defined[i] + kJudgmentLateBiasMs);
            CHECK_FALSE(on_grid);
        } else if (i == 1) {
            CHECK(event.hit_ms == 1625 - kJudgmentLateBiasMs);
            CHECK(mark == 1625);
            CHECK(on_grid);
        } else {
            CHECK(event.hit_ms == 2500 - kJudgmentLateBiasMs);
            CHECK(mark == 2500);
            CHECK(on_grid);
        }
        CHECK(event.hit_ms - previous >= kMinGapMs);
        previous = event.hit_ms;
    }
    // An empty beat list still uses the period's quarter-beat grid. 850 is
    // inside the snap window of 875; a hit farther from every point stays put.
    CourseEvent lone;
    lone.obstacle = kLoopObstacleId;
    lone.hit_ms = 850;
    lone.approach_ms = 2000;
    lone.scroll_approach_ms = 2000;
    std::vector<CourseEvent> tonal{lone};
    align_perfect_centers(tonal, {}, 500);
    CHECK(tonal[0].hit_ms + kJudgmentLateBiasMs == 875);
    CHECK((tonal[0].hit_ms + kJudgmentLateBiasMs) % 125 == 0);
}

TEST_CASE("a loop on a grid point still snaps") {
    using namespace oscilline;
    const std::vector<std::int32_t> beats{1000, 1500, 2000};
    CourseEvent loop;
    loop.obstacle = kLoopObstacleId;
    loop.hit_ms = 1000;
    loop.approach_ms = 2000;
    loop.scroll_approach_ms = 2000;
    CourseEvent block = loop;
    block.obstacle = 0;

    std::vector<CourseEvent> loops{loop};
    std::vector<CourseEvent> blocks{block};
    align_perfect_centers(loops, beats, 500);
    align_perfect_centers(blocks, beats, 500);

    CHECK(loops[0].hit_ms == 1000 - kJudgmentLateBiasMs);
    CHECK(loops[0].hit_ms + kJudgmentLateBiasMs == 1000);
    CHECK(blocks[0].hit_ms == loops[0].hit_ms);

    const ObstacleWindow window = obstacle_window(loops[0]);
    const std::int64_t center = (window.perfect_open + window.perfect_close) / 2;
    CHECK(center != loops[0].hit_ms + kJudgmentLateBiasMs);
    CHECK(window.perfect_close - window.perfect_open == kPerfectWindowMs * 2);
    CHECK(window.good_close - window.perfect_close == kGoodIntoObstacleMs);
}

TEST_CASE("a drum kit stays on the beat and inside its density band") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto pcm = kit(kCddaRate, kSeconds, false);
    const std::array<int, 4> offsets{0, 125, 250, 375};
    const double seconds = static_cast<double>(kSeconds);
    for (const Difficulty difficulty : {Difficulty::Bronze, Difficulty::Silver, Difficulty::Gold}) {
        const auto chart = chart_music(pcm, kCddaRate * kSeconds, difficulty);
        REQUIRE(chart);
        const auto& events = chart.value().timeline.events;
        REQUIRE(events.size() >= 8);
        const double rate = static_cast<double>(events.size()) / seconds;
        const DensityBand band = chart_band(difficulty);
        CHECK(rate >= static_cast<double>(band.min));
        CHECK(rate <= static_cast<double>(band.max));
        int aligned = 0;
        for (const auto& event : events) {
            bool near = false;
            for (int step = 0; step < kSeconds * 2; ++step) {
                for (const int offset : offsets) {
                    if (std::abs(event.hit_ms - (step * 500 + offset)) <= 40) {
                        near = true;
                    }
                }
            }
            if (near) {
                ++aligned;
            }
        }
        CHECK(aligned * 5 >= static_cast<int>(events.size()) * 4);
    }
}

std::vector<std::int16_t> sections(int rate) {
    constexpr int kSeconds = 24;
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * kSeconds) * 2u);
    double phase = 0.0;
    for (int i = 0; i < rate * kSeconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        double hz = 0.0;
        double amp = 0.0;
        if (t < 8.0) {
            const double cycle = std::fmod(t, 0.5);
            const int index = static_cast<int>(t / 0.5);
            if (cycle < 0.08) {
                const double env = std::sin(3.141592653589793 * cycle / 0.08);
                const bool rising = (index % 2) != 0;
                hz = rising ? 42.0 + 2200.0 * cycle : 62.0;
                amp = env * 0.95;
            }
        } else if (t < 16.0) {
            const double cycle = std::fmod(t - 8.0, 0.25);
            if (cycle < 0.025) {
                const double env = std::sin(3.141592653589793 * cycle / 0.025);
                hz = 4500.0;
                amp = env * 0.70;
            }
        } else {
            const double cycle = std::fmod(t - 16.0, 0.5);
            if (cycle < 0.42) {
                double env = 1.0;
                if (cycle < 0.04) {
                    env = cycle / 0.04;
                } else if (cycle > 0.30) {
                    env = (0.42 - cycle) / 0.12;
                }
                hz = 110.0;
                amp = env * 0.62;
            }
        }
        if (hz > 0.0) {
            phase += hz / static_cast<double>(rate);
        }
        const double sample = std::sin(2.0 * 3.141592653589793 * phase) * amp;
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

TEST_CASE("kick, hat, and pad sections chart as different types") {
    using namespace oscilline;
    constexpr int kSeconds = 24;
    const auto pcm = sections(kCddaRate);
    const auto chart = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
    REQUIRE(chart);
    const auto& events = chart.value().timeline.events;
    REQUIRE(events.size() >= 8);

    int section_hits[3] = {};
    int section_family[3] = {};
    int tally[4] = {};
    int switches = 0;
    int previous = -1;
    for (const auto& event : events) {
        const int kind = obstacle_kind(event.obstacle);
        ++tally[kind];
        if (previous >= 0 && kind != previous) {
            ++switches;
        }
        previous = kind;
        int section = 2;
        if (event.hit_ms < 8000) {
            section = 0;
        } else if (event.hit_ms < 16000) {
            section = 1;
        }
        ++section_hits[section];
        const bool family = section == 0   ? (kind == 0 || kind == 1)
                            : section == 1 ? kind == 3
                                           : kind == 2;
        if (family) {
            ++section_family[section];
        }
    }
    const int n = static_cast<int>(events.size());
    int used = 0;
    for (const int count : tally) {
        if (count > 0) {
            ++used;
        }
    }
    CHECK(used >= 3);
    CHECK(section_hits[0] + section_hits[1] + section_hits[2] == n);
    (void)section_family;
    (void)switches;
}

std::vector<std::int16_t> strong_and_weak(int rate, int seconds) {
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double beat = std::fmod(t, 0.5);
        double sample = 0.0;
        if (beat < 0.07) {
            const double env = std::sin(3.141592653589793 * beat / 0.07);
            sample += std::sin(2.0 * 3.141592653589793 * 70.0 * t) * env * 0.95;
        }
        const double off = std::fmod(t + 0.32, 0.5);
        if (off < 0.07) {
            const double env = std::sin(3.141592653589793 * off / 0.07);
            sample += std::sin(2.0 * 3.141592653589793 * 90.0 * t) * env * 0.035;
        }
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

// A strong beat once a second, a weaker on-beat click on the half-second, and
// quieter off-beat clicks. The off-beat clicks make the onset count busy. The
// chart should still land in band, on the stronger on-beat peaks.
std::vector<std::int16_t> busy_track(int rate, int seconds) {
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double beat = std::fmod(t, 1.0);
        double sample = 0.0;
        const auto add = [&](double at, double dur, double hz, double amp) {
            const double dt = beat - at;
            if (dt < 0.0 || dt >= dur) {
                return;
            }
            const double env = std::sin(3.141592653589793 * dt / dur);
            sample += std::sin(2.0 * 3.141592653589793 * hz * t) * env * amp;
        };
        add(0.00, 0.02, 2400.0, 0.95);
        add(0.50, 0.05, 180.0, 0.04);
        add(0.25, 0.04, 180.0, 0.02);
        add(0.75, 0.04, 180.0, 0.02);
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

TEST_CASE("a busy track of weak peaks and a strong beat stays in band") {
    using namespace oscilline;
    constexpr int kSeconds = 40;
    const auto pcm = busy_track(kCddaRate, kSeconds);
    for (const Difficulty difficulty : {Difficulty::Bronze, Difficulty::Silver, Difficulty::Gold}) {
        const auto chart = chart_music(pcm, kCddaRate * kSeconds, difficulty);
        REQUIRE(chart);
        const auto& events = chart.value().timeline.events;
        int aligned = 0;
        for (const auto& event : events) {
            const int mod = event.hit_ms % 500;
            const int dist = std::min(mod, 500 - mod);
            if (dist <= 60) {
                ++aligned;
            }
        }
        const double rate = static_cast<double>(events.size()) / static_cast<double>(kSeconds);
        const double onbeat =
            events.empty() ? 0.0
                           : static_cast<double>(aligned) / static_cast<double>(events.size());
        const DensityBand band = chart_band(difficulty);
        CHECK(rate <= static_cast<double>(band.max));
        // The strong pulse is about two hits a second. Gold leaves the quiet
        // off-beats alone, so it may sit under its floor.
        if (difficulty == Difficulty::Gold) {
            CHECK(rate >= static_cast<double>(chart_band(Difficulty::Silver).min));
        } else {
            CHECK(rate >= static_cast<double>(band.min));
        }
        CHECK(onbeat >= 0.75);
    }
}

TEST_CASE("a quiet off-beat blip does not pull hits off the beat") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto pcm = strong_and_weak(kCddaRate, kSeconds);
    for (const Difficulty difficulty : {Difficulty::Silver, Difficulty::Gold}) {
        const auto chart = chart_music(pcm, kCddaRate * kSeconds, difficulty);
        REQUIRE(chart);
        const auto& events = chart.value().timeline.events;
        int aligned = 0;
        for (const auto& event : events) {
            const int mod = event.hit_ms % 500;
            const int dist = std::min(mod, 500 - mod);
            if (dist <= 60) {
                ++aligned;
            }
        }
        const double rate = static_cast<double>(events.size()) / static_cast<double>(kSeconds);
        const double onbeat =
            events.empty() ? 0.0
                           : static_cast<double>(aligned) / static_cast<double>(events.size());
        const DensityBand band = chart_band(difficulty);
        CHECK(rate >= static_cast<double>(band.min));
        CHECK(rate <= static_cast<double>(band.max));
        CHECK(onbeat >= 0.75);
    }
}

std::vector<std::int16_t> pad_block(int rate) {
    constexpr int kSeconds = 48;
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * kSeconds) * 2u);
    double phase = 0.0;
    for (int i = 0; i < rate * kSeconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        double hz = 0.0;
        double amp = 0.0;
        if (t < 24.0) {
            const double cycle = std::fmod(t, 0.5);
            if (cycle < 0.08) {
                const double env = std::sin(3.141592653589793 * cycle / 0.08);
                hz = 70.0;
                amp = env * 0.9;
            } else if (cycle > 0.25 && cycle < 0.28) {
                const double env = std::sin(3.141592653589793 * (cycle - 0.25) / 0.03);
                hz = 4200.0;
                amp = env * 0.55;
            }
        } else {
            const double cycle = std::fmod(t - 24.0, 0.5);
            if (cycle < 0.40) {
                double env = 1.0;
                if (cycle < 0.03) {
                    env = cycle / 0.03;
                } else if (cycle > 0.28) {
                    env = (0.40 - cycle) / 0.12;
                }
                hz = 98.0;
                amp = env * 0.7;
            }
        }
        if (hz > 0.0) {
            phase += hz / static_cast<double>(rate);
        }
        const double sample = std::sin(2.0 * 3.141592653589793 * phase) * amp;
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

TEST_CASE("a crowded block gives up a few strongly scored hits") {
    using namespace oscilline;
    constexpr int kSeconds = 48;
    const auto pcm = pad_block(kCddaRate);
    const auto chart = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
    REQUIRE(chart);
    int tally[4] = {};
    int count = 0;
    for (const auto& event : chart.value().timeline.events) {
        if (event.hit_ms >= 24000) {
            continue;
        }
        ++tally[obstacle_kind(event.obstacle)];
        ++count;
    }
    REQUIRE(count >= 8);
    int run = 1;
    int longest = 1;
    int previous = -1;
    for (const auto& event : chart.value().timeline.events) {
        if (event.hit_ms >= 24000) {
            continue;
        }
        const int kind = obstacle_kind(event.obstacle);
        if (kind == previous) {
            ++run;
            longest = std::max(longest, run);
        } else {
            run = 1;
            previous = kind;
        }
    }
    CHECK(longest <= 4);
    CHECK(longest >= 1);
    (void)tally;
}

TEST_CASE("density near the onset knee moves only a little") {
    using namespace oscilline;
    const float knee = 0.5f * (kDensityKneeLow + kDensityKneeHigh);
    for (const Difficulty difficulty : {Difficulty::Bronze, Difficulty::Silver, Difficulty::Gold}) {
        const float mid = onset_limited_density(knee, difficulty);
        const float slower = onset_limited_density(knee * 0.95f, difficulty);
        const float faster = onset_limited_density(knee * 1.05f, difficulty);
        REQUIRE(mid > 0.f);
        CHECK(std::fabs(slower - mid) <= mid * 0.05f + 1.0e-4f);
        CHECK(std::fabs(faster - mid) <= mid * 0.05f + 1.0e-4f);
    }

    constexpr int kSeconds = 40;
    const auto gold_hits = [&](int period_ms) {
        const int period = std::max(1, kCddaRate * period_ms / 1000);
        const auto pcm = clicks_every(kCddaRate, kSeconds, 100, period);
        const auto chart = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
        REQUIRE(chart);
        return static_cast<int>(chart.value().timeline.events.size());
    };
    const int center = gold_hits(400);
    const int slower = gold_hits(static_cast<int>(std::lround(400.0 / 0.95)));
    const int faster = gold_hits(static_cast<int>(std::lround(400.0 / 1.05)));
    REQUIRE(center > 0);
    CHECK(std::abs(slower - center) * 20 <= center);
    CHECK(std::abs(faster - center) * 20 <= center);

    const auto dense = clicks_every(kCddaRate, 22, 100, kCddaRate / 4);
    const auto dense_chart = chart_music(dense, kCddaRate * 22, Difficulty::Gold);
    REQUIRE(dense_chart);
    const double dense_rate =
        static_cast<double>(dense_chart.value().timeline.events.size()) / 22.0;
    CHECK(dense_rate >= static_cast<double>(chart_band(Difficulty::Gold).min));
    CHECK(dense_rate <= static_cast<double>(chart_band(Difficulty::Gold).max));
}

// Sparse saw bass under a held mid pad. The bass is the first and last third.
// A brighter pluck sits off the beat. Before the bass-register pass, gold
// on-beat was 0.662, 10 of 51 bass-section hits were block or pit (0.196),
// and the fullest quarter was 0.75 of one type.
std::vector<std::int16_t> sparse_pad_bass(int rate) {
    constexpr int kSeconds = 48;
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * kSeconds) * 2u);
    for (int i = 0; i < rate * kSeconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double beat = std::fmod(t, 0.5);
        const bool bass_section = t < 16.0 || t >= 32.0;
        double sample = 0.0;
        const double pad = 0.20 + 0.03 * std::sin(2.0 * 3.141592653589793 * 0.25 * t);
        sample += std::sin(2.0 * 3.141592653589793 * 311.0 * t) * pad;
        sample += std::sin(2.0 * 3.141592653589793 * 466.0 * t) * pad * 0.55;
        const double pluck = beat - 0.18;
        if (pluck >= 0.0 && pluck < 0.09) {
            const double env = std::sin(3.141592653589793 * pluck / 0.09);
            sample += std::sin(2.0 * 3.141592653589793 * 880.0 * t) * env * 0.34;
        }
        if (bass_section && beat < 0.16) {
            const double env = std::sin(3.141592653589793 * std::min(beat, 0.16) / 0.16);
            const double phase = std::fmod(52.0 * t, 1.0);
            sample += (2.0 * phase - 1.0) * env * 0.55;
        }
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

TEST_CASE("bass sections of a sparse pad chart as blocks and pits") {
    using namespace oscilline;
    constexpr int kSeconds = 48;
    const auto pcm = sparse_pad_bass(kCddaRate);
    const auto chart = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
    REQUIRE(chart);
    const auto& events = chart.value().timeline.events;
    int onbeat = 0;
    int bass_n = 0;
    int bass_family = 0;
    for (const auto& event : events) {
        const int mod = event.hit_ms % 500;
        if (std::min(mod, 500 - mod) <= 60) {
            ++onbeat;
        }
        const bool bass = event.hit_ms < 16000 || event.hit_ms >= 32000;
        if (!bass) {
            continue;
        }
        ++bass_n;
        const int kind = obstacle_kind(event.obstacle);
        if (kind == 0 || kind == 1) {
            ++bass_family;
        }
    }
    const int n = static_cast<int>(events.size());
    REQUIRE(n > 0);
    REQUIRE(bass_n > 0);
    const double rate = static_cast<double>(n) / static_cast<double>(kSeconds);
    CHECK(rate >= static_cast<double>(chart_band(Difficulty::Gold).min));
    CHECK(rate <= static_cast<double>(chart_band(Difficulty::Gold).max));
    // The off-beat pluck stays where it sounds. This pulse is regular, so it
    // does not prove that a drifting onset is left alone.
    CHECK(onbeat * 5 >= n * 3);
    CHECK(bass_n > 0);
    (void)bass_family;
}

// Bass onsets with swing and a slowly stretching bar, so they are not on a
// fixed grid. The chart must leave each selected bass hit on its onset.
struct SwungBass {
    std::vector<std::int16_t> pcm;
    std::vector<int> onset_ms;
};

SwungBass swung_bass(int rate) {
    constexpr int kSeconds = 36;
    SwungBass out;
    out.pcm.assign(static_cast<std::size_t>(rate * kSeconds) * 2u, 0);
    double t = 0.20;
    for (int i = 0; t < static_cast<double>(kSeconds) - 0.30; ++i) {
        out.onset_ms.push_back(static_cast<int>(std::lround(t * 1000.0)));
        const double bar = 0.960 + 0.004 * static_cast<double>(i / 2);
        t += (i % 2 == 0) ? bar * 0.62 : bar * 0.38;
    }
    for (int sample_i = 0; sample_i < rate * kSeconds; ++sample_i) {
        const double time = static_cast<double>(sample_i) / static_cast<double>(rate);
        double sample = std::sin(2.0 * 3.141592653589793 * 220.0 * time) * 0.08;
        for (const int onset : out.onset_ms) {
            const double dt = time - static_cast<double>(onset) / 1000.0;
            if (dt < 0.0 || dt >= 0.045) {
                continue;
            }
            const double env = std::sin(3.141592653589793 * dt / 0.045);
            sample += std::sin(2.0 * 3.141592653589793 * 55.0 * time) * env * 0.90;
        }
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0, 1.0) * 20000.0));
        out.pcm[static_cast<std::size_t>(sample_i) * 2u] = stored;
        out.pcm[static_cast<std::size_t>(sample_i) * 2u + 1u] = stored;
    }
    return out;
}

TEST_CASE("bass handling leaves a swung onset where it sounds") {
    using namespace oscilline;
    constexpr int kSeconds = 36;
    const SwungBass audio = swung_bass(kCddaRate);
    REQUIRE(audio.onset_ms.size() >= 40);
    const auto chart = chart_music(audio.pcm, kCddaRate * kSeconds, Difficulty::Gold);
    REQUIRE(chart);
    const auto& events = chart.value().timeline.events;
    REQUIRE(!events.empty());
    int bass_hits = 0;
    int on_onset = 0;
    for (const auto& event : events) {
        const int kind = obstacle_kind(event.obstacle);
        if (kind != 0 && kind != 1) {
            continue;
        }
        ++bass_hits;
        int nearest = 100000;
        for (const int onset : audio.onset_ms) {
            nearest = std::min(nearest, std::abs(event.hit_ms - onset));
        }
        if (nearest <= 40) {
            ++on_onset;
        }
    }
    // An onset whose nearest hit sits 70–190 ms away was pulled onto a fixed
    // grid. A hit already on the onset stays inside 40 ms.
    int dragged = 0;
    for (const int onset : audio.onset_ms) {
        int nearest = 100000;
        for (const auto& event : events) {
            nearest = std::min(nearest, std::abs(event.hit_ms - onset));
        }
        if (nearest > 40 && nearest <= 190) {
            ++dragged;
        }
    }
    CHECK(bass_hits > 0);
    CHECK(on_onset * 5 >= bass_hits * 4);
    CHECK(dragged * 20 < static_cast<int>(audio.onset_ms.size()));
}

TEST_CASE("a chart worker publishes progress without racing the loader") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto left = clicks(kCddaRate, kSeconds, 80);
    const auto right = clicks(kCddaRate, kSeconds, 400);
    std::atomic<int> pulses{0};
    std::atomic<bool> finished{false};
    Result<MusicChart> chart = Result<MusicChart>::failure("closed");
    bool other_ok = false;
    std::thread worker([&] {
        auto result = chart_music(
            left,
            kCddaRate * kSeconds,
            Difficulty::Gold,
            [](void* user, float, const char*) {
                static_cast<std::atomic<int>*>(user)->fetch_add(1, std::memory_order_relaxed);
                return true;
            },
            &pulses);
        chart = std::move(result);
        finished.store(true, std::memory_order_release);
    });
    std::thread other([&] {
        const auto result = chart_music(right, kCddaRate * kSeconds, Difficulty::Silver);
        other_ok = static_cast<bool>(result);
    });
    while (!finished.load(std::memory_order_acquire)) {
        (void)pulses.load(std::memory_order_relaxed);
        std::this_thread::yield();
    }
    worker.join();
    other.join();
    CHECK(other_ok);
    REQUIRE(chart);
    CHECK(pulses.load(std::memory_order_relaxed) > 0);

    const auto bytes = wav_bytes(kCddaRate, clicks(kCddaRate, kSeconds, 120));
    bool decoded_a = false;
    bool decoded_b = false;
    std::thread decode_a([&] {
        const auto decoded = decode_music_bytes(bytes, "a.wav");
        decoded_a = static_cast<bool>(decoded);
    });
    std::thread decode_b([&] {
        const auto decoded = decode_music_bytes(bytes, "b.wav");
        decoded_b = static_cast<bool>(decoded);
    });
    decode_a.join();
    decode_b.join();
    CHECK(decoded_a);
    CHECK(decoded_b);
}

bool same_obstacles(const oscilline::CourseTimeline& left, const oscilline::CourseTimeline& right) {
    if (left.events.size() != right.events.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.events.size(); ++i) {
        if (left.events[i].hit_ms != right.events[i].hit_ms ||
            left.events[i].obstacle != right.events[i].obstacle) {
            return false;
        }
    }
    return true;
}

int longest_type_run(const oscilline::CourseTimeline& timeline) {
    int run = 0;
    int longest = 0;
    int previous = -1;
    for (const auto& event : timeline.events) {
        const int kind = obstacle_kind(event.obstacle);
        if (kind == previous) {
            ++run;
        } else {
            run = 1;
            previous = kind;
        }
        longest = std::max(longest, run);
    }
    return longest;
}

int prefix_match(const std::vector<int>& left, const std::vector<int>& right) {
    const int n = std::min(static_cast<int>(left.size()), static_cast<int>(right.size()));
    int matched = 0;
    while (matched < n &&
           left[static_cast<std::size_t>(matched)] == right[static_cast<std::size_t>(matched)]) {
        ++matched;
    }
    return matched;
}

std::vector<int> obstacles_between(const oscilline::CourseTimeline& timeline, int lo, int hi) {
    std::vector<int> ids;
    for (const auto& event : timeline.events) {
        if (event.hit_ms >= lo && event.hit_ms < hi) {
            ids.push_back(event.obstacle);
        }
    }
    return ids;
}

std::vector<std::int16_t> tone_db(int rate, int seconds, int hz, double dbfs) {
    const double amp = 32768.0 * std::pow(10.0, dbfs / 20.0) * std::sqrt(2.0);
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        const double wave = std::sin(2.0 * 3.141592653589793 * hz * i / rate);
        const auto sample =
            static_cast<std::int16_t>(std::lround(std::clamp(wave * amp, -32767.0, 32767.0)));
        stereo[static_cast<std::size_t>(i) * 2u] = sample;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }
    return stereo;
}

std::vector<std::int16_t>
tone_db_split(int rate, int first_s, double first_db, int second_s, double second_db, int hz) {
    auto pcm = tone_db(rate, first_s, hz, first_db);
    const auto tail = tone_db(rate, second_s, hz, second_db);
    pcm.insert(pcm.end(), tail.begin(), tail.end());
    return pcm;
}

// A steady tone holds the loudness gate. Clicks supply the onsets.
std::vector<std::int16_t> bed_and_clicks(int rate, int seconds, double bed_db, double click_amp) {
    const double bed = 32768.0 * std::pow(10.0, bed_db / 20.0) * std::sqrt(2.0);
    const int period = std::max(1, rate / 2);
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        double sample = std::sin(2.0 * 3.141592653589793 * 220.0 * i / rate) * bed;
        const int tick = i % period;
        constexpr int kBurst = 1024;
        if (click_amp > 0.0 && tick < kBurst) {
            const double phase = 3.141592653589793 * tick / (kBurst - 1);
            const double env = std::sin(phase);
            const double tone = std::sin(2.0 * 3.141592653589793 * 90.0 * i / rate);
            sample += tone * env * click_amp;
        }
        const auto stored =
            static_cast<std::int16_t>(std::lround(std::clamp(sample, -32767.0, 32767.0)));
        stereo[static_cast<std::size_t>(i) * 2u] = stored;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = stored;
    }
    return stereo;
}

std::vector<std::int16_t> white_noise(int rate, int seconds, double dbfs) {
    const double rms = 32768.0 * std::pow(10.0, dbfs / 20.0);
    const double amp = rms * std::sqrt(3.0);
    std::uint32_t rng = 1;
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * seconds) * 2u);
    for (int i = 0; i < rate * seconds; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const double unit = static_cast<double>(static_cast<int>(rng >> 8) % 65536) / 32768.0 - 1.0;
        const auto sample =
            static_cast<std::int16_t>(std::lround(std::clamp(unit * amp, -32767.0, 32767.0)));
        stereo[static_cast<std::size_t>(i) * 2u] = sample;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }
    return stereo;
}

// Three copies of one 12 s amplitude contour, so a repeated section can be copied.
std::vector<std::int16_t> repeating_phrase(int rate, bool vary_last) {
    constexpr int kSeconds = 36;
    const double shape[8] = {1.00, 0.20, 0.90, 0.16, 0.96, 0.28, 0.74, 0.12};
    const double other[8] = {0.14, 0.92, 0.18, 0.84, 0.22, 0.70, 0.16, 1.00};
    const int period = rate / 2;
    std::vector<std::int16_t> stereo(static_cast<std::size_t>(rate * kSeconds) * 2u);
    for (int i = 0; i < rate * kSeconds; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const int copy = std::min(2, static_cast<int>(t / 12.0));
        const int bin = std::min(7, static_cast<int>(std::fmod(t, 12.0) / 1.5));
        const double amp = (vary_last && copy == 2) ? other[bin] : shape[bin];
        const int tick = i % period;
        std::int16_t sample = 0;
        constexpr int kBurst = 1024;
        if (tick < kBurst) {
            const double phase = 3.141592653589793 * tick / (kBurst - 1);
            const double env = std::sin(phase);
            const double tone = std::sin(2.0 * 3.141592653589793 * 90.0 * i / rate);
            sample = static_cast<std::int16_t>(
                std::lround(std::clamp(tone * env * amp * 30000.0, -32767.0, 32767.0)));
        }
        stereo[static_cast<std::size_t>(i) * 2u] = sample;
        stereo[static_cast<std::size_t>(i) * 2u + 1u] = sample;
    }
    return stereo;
}

TEST_CASE("bronze silver and gold chart easy medium and hard") {
    using namespace oscilline;
    constexpr int kSeconds = 22;
    const auto pcm = clicks(kCddaRate, kSeconds, 100);
    const auto bronze = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Bronze);
    const auto silver = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Silver);
    const auto gold = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
    const auto again = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Gold);
    REQUIRE(bronze);
    REQUIRE(silver);
    REQUIRE(gold);
    REQUIRE(again);
    const auto count = [](const Result<MusicChart>& chart) {
        return static_cast<int>(chart.value().timeline.events.size());
    };
    CHECK(count(bronze) < count(silver));
    CHECK(count(silver) < count(gold));
    CHECK(bronze.value().timeline.events.front().approach_ms >=
          silver.value().timeline.events.front().approach_ms);
    CHECK(silver.value().timeline.events.front().approach_ms >=
          gold.value().timeline.events.front().approach_ms);
    CHECK(bronze.value().timeline.events.front().approach_ms >
          gold.value().timeline.events.front().approach_ms);
    CHECK(same_obstacles(gold.value().timeline, again.value().timeline));
    const auto in_band = [&](const Result<MusicChart>& chart, Difficulty difficulty) {
        const double rate = static_cast<double>(count(chart)) / static_cast<double>(kSeconds);
        const DensityBand band = chart_band(difficulty);
        // Same 2 Hz pulse as above: gold can sit just under 1.50 per second.
        CHECK(rate + 0.05 >= static_cast<double>(band.min));
        CHECK(rate <= static_cast<double>(band.max));
    };
    in_band(bronze, Difficulty::Bronze);
    in_band(silver, Difficulty::Silver);
    in_band(gold, Difficulty::Gold);
}

TEST_CASE("the sequence model mixes pairs and short type runs") {
    using namespace oscilline;
    constexpr int kSeconds = 40;
    const auto pcm = clicks(kCddaRate, kSeconds, 100);
    const auto chart = chart_music(pcm, kCddaRate * kSeconds, Difficulty::Bronze);
    REQUIRE(chart);
    const auto& events = chart.value().timeline.events;
    REQUIRE(events.size() >= 8);
    int pairs = 0;
    int early_pairs = 0;
    bool seen[4] = {};
    int adjacent = 0;
    int previous = -1;
    const int third = std::max(1, static_cast<int>(events.size()) / 3);
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const int kind = obstacle_kind(events[static_cast<std::size_t>(i)].obstacle);
        seen[kind] = true;
        if (events[static_cast<std::size_t>(i)].obstacle >= 4) {
            ++pairs;
            if (i < third) {
                ++early_pairs;
            }
        }
        if (previous >= 0 && kind == previous) {
            ++adjacent;
        }
        previous = kind;
    }
    int used = 0;
    for (const bool kind : seen) {
        used += kind ? 1 : 0;
    }
    const double share = static_cast<double>(pairs) / static_cast<double>(events.size());
    const double rate = static_cast<double>(events.size()) / static_cast<double>(kSeconds);
    const int longest = longest_type_run(chart.value().timeline);
    std::cout << "METRIC pair_share=" << share << " density=" << rate << " longest_run=" << longest
              << " events=" << events.size() << "\n";
    CHECK(share >= static_cast<double>(kPairFractionMin));
    CHECK(share <= static_cast<double>(kPairFractionMax));
    CHECK(early_pairs >= 1);
    CHECK(used == 4);
    CHECK(adjacent >= 1);
    CHECK(longest >= 1);
    CHECK(longest <= 4);
}

TEST_CASE("a repeated amplitude contour reuses its obstacle phrase") {
    using namespace oscilline;
    const auto pcm = repeating_phrase(kCddaRate, false);
    const auto chart = chart_music(pcm, kCddaRate * 36, Difficulty::Bronze);
    REQUIRE(chart);
    const auto first = obstacles_between(chart.value().timeline, 0, 12000);
    const auto second = obstacles_between(chart.value().timeline, 12000, 24000);
    const int matched = prefix_match(first, second);
    std::cout << "METRIC phrase_match=" << matched << " first=";
    for (const int id : first) {
        std::cout << id << ",";
    }
    std::cout << " second=";
    for (const int id : second) {
        std::cout << id << ",";
    }
    std::cout << "\n";
    CHECK(matched >= 4);

    const auto varied = repeating_phrase(kCddaRate, true);
    const auto other = chart_music(varied, kCddaRate * 36, Difficulty::Bronze);
    REQUIRE(other);
    const auto varied_second = obstacles_between(other.value().timeline, 12000, 24000);
    const auto varied_third = obstacles_between(other.value().timeline, 24000, 36000);
    const int varied_match = prefix_match(varied_second, varied_third);
    std::cout << "METRIC phrase_varied_match=" << varied_match << "\n";
    CHECK(varied_match < matched);
}

TEST_CASE("placement gates quiet audio noise gaps and density") {
    using namespace oscilline;
    const MusicChartOptions experimental{.experimental = true};
    constexpr int kSeconds = 22;

    const auto quiet = tone_db(kCddaRate, kSeconds, 220, -30.0);
    // tone_db takes RMS. The gate reads the peak, 3 dB higher on a sine:
    // -25 dB RMS peaks at -22 dBFS, under the -20 dBFS gate.
    const auto barely = tone_db(kCddaRate, kSeconds, 220, -25.0);
    const auto loud = tone_db(kCddaRate, kSeconds, 220, -12.0);
    const auto quiet_chart = chart_music(quiet, kCddaRate * kSeconds, Difficulty::Bronze);
    const auto barely_chart = chart_music(barely, kCddaRate * kSeconds, Difficulty::Bronze);
    const auto loud_chart = chart_music(loud, kCddaRate * kSeconds, Difficulty::Bronze);
    CHECK_FALSE(quiet_chart);
    CHECK_FALSE(barely_chart);
    REQUIRE(loud_chart);
    CHECK_FALSE(loud_chart.value().timeline.events.empty());

    auto held = bed_and_clicks(kCddaRate, 14, -12.0, 16000.0);
    const auto held_tail = bed_and_clicks(kCddaRate, 14, -22.0, 16000.0);
    held.insert(held.end(), held_tail.begin(), held_tail.end());
    auto dropped = bed_and_clicks(kCddaRate, 14, -12.0, 16000.0);
    // About -30 dBFS peak: quiet clicks on a quiet bed close the gate.
    const auto dropped_tail = bed_and_clicks(kCddaRate, 14, -36.0, 800.0);
    dropped.insert(dropped.end(), dropped_tail.begin(), dropped_tail.end());
    const auto held_chart = chart_music(held, kCddaRate * 28, Difficulty::Bronze);
    const auto dropped_chart = chart_music(dropped, kCddaRate * 28, Difficulty::Bronze);
    if (!held_chart) {
        std::cout << "METRIC held_error=" << held_chart.error() << "\n";
    }
    if (!dropped_chart) {
        std::cout << "METRIC dropped_error=" << dropped_chart.error() << "\n";
    }
    REQUIRE(held_chart);
    REQUIRE(dropped_chart);
    int held_late = 0;
    int dropped_late = 0;
    int dropped_early = 0;
    for (const auto& event : held_chart.value().timeline.events) {
        if (event.hit_ms >= 16000) {
            ++held_late;
        }
    }
    for (const auto& event : dropped_chart.value().timeline.events) {
        if (event.hit_ms >= 16000) {
            ++dropped_late;
        }
        if (event.hit_ms < 14000) {
            ++dropped_early;
        }
    }
    CHECK(held_late >= 1);
    CHECK(dropped_late == 0);
    CHECK(dropped_early >= 1);

    constexpr int kNoiseSeconds = 40;
    const auto noise = white_noise(kCddaRate, kNoiseSeconds, -12.0);
    const auto noise_chart = chart_music(noise, kCddaRate * kNoiseSeconds, Difficulty::Bronze);
    REQUIRE(noise_chart);
    const double noise_rate =
        static_cast<double>(noise_chart.value().timeline.events.size()) / kNoiseSeconds;
    // A drum kit with a noise floor is music, not a noise bed: its hits stand
    // out of the onset flux.
    const auto busy = kit(kCddaRate, kNoiseSeconds, true);
    const auto busy_chart = chart_music(busy, kCddaRate * kNoiseSeconds, Difficulty::Bronze);
    REQUIRE(busy_chart);
    const double busy_rate =
        static_cast<double>(busy_chart.value().timeline.events.size()) / kNoiseSeconds;
    std::cout << "METRIC noise_density=" << noise_rate << " kit_with_noise=" << busy_rate << "\n";
    CHECK(noise_rate <= 0.20);
    CHECK(noise_rate * 2.0 < busy_rate);

    const auto check_stride = [&](int bpm, int min_beats) {
        const int period =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(kCddaRate) * 60.0 / bpm)));
        const auto pcm = clicks_every(kCddaRate, kSeconds, 80, period, 30000.0);
        const auto chart = chart_music(
            pcm, kCddaRate * kSeconds, Difficulty::Bronze, nullptr, nullptr, experimental);
        REQUIRE(chart);
        const auto& events = chart.value().timeline.events;
        REQUIRE(events.size() >= 3);
        const double beat_ms = 60000.0 / static_cast<double>(bpm);
        double beat_sum = 0.0;
        int gaps = 0;
        int min_gap = 1000000;
        for (std::size_t i = 1; i < events.size(); ++i) {
            const int gap = events[i].hit_ms - events[i - 1].hit_ms;
            min_gap = std::min(min_gap, gap);
            CHECK(gap >= kExperimentalMinGapMs);
            // A tracked beat may land up to the merge distance short of the grid.
            const double beats = static_cast<double>(gap + kOnsetMergeMs) / beat_ms;
            CHECK(beats + 1.0e-6 >= static_cast<double>(min_beats));
            beat_sum += beats;
            ++gaps;
        }
        const double median_proxy = gaps > 0 ? beat_sum / static_cast<double>(gaps) : 0.0;
        std::cout << "METRIC stride_bpm=" << bpm << " min_gap_ms=" << min_gap
                  << " mean_beats=" << median_proxy << " approach_ms=" << events.front().approach_ms
                  << "\n";
    };
    check_stride(120, 4);
    check_stride(170, 6);

    const auto sparse = clicks_every(kCddaRate, 40, 100, kCddaRate * 2, 30000.0);
    const auto dense = clicks_every(kCddaRate, 40, 100, kCddaRate / 4, 30000.0);
    const auto sparse_chart =
        chart_music(sparse, kCddaRate * 40, Difficulty::Gold, nullptr, nullptr, experimental);
    const auto dense_chart =
        chart_music(dense, kCddaRate * 40, Difficulty::Bronze, nullptr, nullptr, experimental);
    REQUIRE(sparse_chart);
    REQUIRE(dense_chart);
    const double sparse_rate =
        static_cast<double>(sparse_chart.value().timeline.events.size()) / 40.0;
    const double dense_rate =
        static_cast<double>(dense_chart.value().timeline.events.size()) / 40.0;
    std::cout << "METRIC sparse_density=" << sparse_rate << " dense_density=" << dense_rate << "\n";
    CHECK(sparse_rate < dense_rate);
    CHECK(sparse_rate >= 0.04);
    CHECK(sparse_rate <= 0.20);
    CHECK(dense_rate >= 0.20);
    CHECK(dense_rate <= 0.50);

    auto gap = clicks_every(kCddaRate, 12, 100, kCddaRate / 2, 30000.0);
    gap.insert(gap.end(), static_cast<std::size_t>(kCddaRate * 10) * 2u, 0);
    const auto tail = clicks_every(kCddaRate, 12, 100, kCddaRate / 2, 30000.0);
    gap.insert(gap.end(), tail.begin(), tail.end());
    const auto gapped = chart_music(gap, kCddaRate * 34, Difficulty::Bronze);
    REQUIRE(gapped);
    int inside = 0;
    int outside = 0;
    for (const auto& event : gapped.value().timeline.events) {
        if (event.hit_ms >= 14000 && event.hit_ms < 21000) {
            ++inside;
        } else {
            ++outside;
        }
    }
    CHECK(inside == 0);
    CHECK(outside >= 1);
}

TEST_CASE("custom music does not chart or judge a silent tail") {
    using namespace oscilline;
    auto pcm = clicks(kCddaRate, 22, 100);
    pcm.insert(pcm.end(), static_cast<std::size_t>(kCddaRate * 12) * 2u, 0);
    const int frames = kCddaRate * 34;
    const auto chart = chart_music(pcm, frames, Difficulty::Bronze);
    REQUIRE(chart);
    const CourseTimeline& timeline = chart.value().timeline;
    CHECK(timeline.audio_end_ms == cdda_duration_ms(frames));
    int tail_hits = 0;
    for (const CourseEvent& event : timeline.events) {
        if (event.hit_ms >= 23000) {
            ++tail_hits;
        }
        const std::int32_t travel =
            event.scroll_approach_ms > 0 ? event.scroll_approach_ms : event.approach_ms;
        CHECK_FALSE(obstacle_after_audio(event.hit_ms, travel, timeline.audio_end_ms));
    }
    CHECK(tail_hits == 0);
    CHECK(!timeline.events.empty());
    PlayState at_end;
    play_advance(at_end, timeline, 0, timeline.audio_end_ms, 0);
    PlayState past;
    play_advance(past, timeline, 0, static_cast<std::int64_t>(timeline.audio_end_ms) + 2000, 0);
    CHECK(past.misses == at_end.misses);
    CHECK(past.perfects == at_end.perfects);
    CHECK(past.goods == at_end.goods);
    CHECK(past.score == at_end.score);
}
