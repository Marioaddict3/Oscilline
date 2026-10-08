// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// CD-DA decode and the playback clock on a tiny image.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/present/cdda_out.hpp"
#include "support/iso_builder.hpp"

#include <SDL3/SDL.h>
#include <cstdint>
#include <doctest/doctest.h>
#include <map>
#include <string>
#include <vector>

TEST_CASE("a long track is capped at fifteen minutes") {
    CHECK(oscilline::cdda_frames_for_sectors(0) == 0);
    CHECK(oscilline::cdda_frames_for_sectors(2) == 2 * oscilline::kCddaFramesPerSector);
    const int cap = oscilline::kMaxCddaSeconds * oscilline::kCddaRate;
    CHECK(oscilline::cdda_frames_for_sectors(1000000) == cap);
    CHECK(oscilline::cdda_duration_ms(1176) == 1176 * 1000 / oscilline::kCddaRate);
}

TEST_CASE("CD-DA sectors decode as 44.1 kHz stereo frames") {
    std::vector<std::uint8_t> data(10u * 2352u, 0);
    std::vector<std::uint8_t> audio;
    oscilline::testutil::append_cdda(audio, 2, 0x1234);
    const char* cue = "FILE \"data.bin\" BINARY\n"
                      "  TRACK 01 MODE1/2352\n"
                      "    INDEX 01 00:00:00\n"
                      "FILE \"audio.bin\" BINARY\n"
                      "  TRACK 02 AUDIO\n"
                      "    INDEX 01 00:00:00\n";
    std::map<std::string, std::vector<std::uint8_t>> files;
    files.emplace("data.bin", std::move(data));
    files.emplace("audio.bin", std::move(audio));
    auto image = oscilline::DiscImage::open_cue_bytes(cue, std::move(files));
    REQUIRE(image);

    auto track = oscilline::read_cdda_track(image.value(), 2);
    REQUIRE(track);
    CHECK(track.value().frames == 2 * 588);
    REQUIRE(track.value().interleaved.size() ==
            static_cast<std::size_t>(track.value().frames) * 2u);
    CHECK(track.value().interleaved[10 * 2] == 0x1234);
    CHECK(track.value().interleaved[10 * 2 + 1] == 10);
    CHECK(track.value().interleaved[588 * 2 + 1] == 0);
    CHECK(track.value().interleaved[588 * 2] == 0x1234);

    auto missing = oscilline::read_cdda_track(image.value(), 3);
    CHECK_FALSE(missing);
}

TEST_CASE("the silent fallback freezes the start delay when paused") {
    oscilline::CddaOutput::Config config;
    config.start_delay_ms = 1000;
    auto audio = oscilline::CddaOutput::open(std::move(config));
    REQUIRE(audio.position_ms() < 0);
    audio.set_paused(true);
    const auto frozen = audio.position_ms();
    SDL_Delay(25);
    CHECK(audio.position_ms() == frozen);
    audio.set_paused(false);
    SDL_Delay(10);
    CHECK(audio.position_ms() > frozen);
    CHECK(audio.position_ms() < 0);
}

TEST_CASE("a delayed device begins at track zero rather than consuming music in the prelude") {
    REQUIRE(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    oscilline::CddaOutput::Config config;
    config.frames = oscilline::kCddaRate;
    config.interleaved.assign(static_cast<std::size_t>(config.frames) * 2u, 123);
    config.start_delay_ms = 80;
    auto audio = oscilline::CddaOutput::open(std::move(config));
    REQUIRE(audio.device_open());
    REQUIRE(audio.position_ms() < 0);
    audio.set_paused(true);
    const auto frozen = audio.position_ms();
    SDL_Delay(25);
    CHECK(audio.position_ms() == frozen);
    audio.set_paused(false);
    SDL_Delay(120);
    const auto start = audio.position_ms();
    CHECK(start >= 0);
    CHECK(start < 40);
    SDL_Delay(80);
    CHECK(audio.position_ms() > start);
    audio.set_paused(true);
    const auto stopped = audio.position_ms();
    SDL_Delay(25);
    CHECK(audio.position_ms() == stopped);
}
