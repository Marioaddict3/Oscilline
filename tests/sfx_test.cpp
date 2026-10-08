// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Event-map rates, gating, and mixer playback.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/audio/menu_sfx.hpp"
#include "oscilline/audio/sfx.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "sfx_stage.hpp"
#include "support/iso_builder.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class Buf {
  public:
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFF));
        u16(static_cast<std::uint16_t>(value >> 16));
    }

    void zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
};

std::uint64_t fnv1a(std::span<const std::int16_t> samples) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::int16_t sample : samples) {
        const auto bits = static_cast<std::uint16_t>(sample);
        hash ^= static_cast<std::uint8_t>(bits & 0xFF);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>(bits >> 8);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::unique_ptr<oscilline::SfxMixer> make_mixer(const oscilline::SfxLibrary* library = nullptr,
                                                bool experimental = false) {
    oscilline::SfxConfig config;
    config.experimental = experimental;
    return std::make_unique<oscilline::SfxMixer>(
        library,
        std::span<const oscilline::SfxMapEntry>(oscilline::kSfxMap,
                                                static_cast<std::size_t>(oscilline::SfxId::Count)),
        config);
}

Buf tone_header(std::uint16_t adsr1, std::uint16_t adsr2, int vag_units) {
    Buf header;
    header.bytes.insert(header.bytes.end(), {'p', 'B', 'A', 'V'});
    header.u32(1);
    header.u32(1);
    header.u32(1);
    header.u16(0);
    header.u16(1);
    header.u16(1);
    header.u16(1);
    header.u8(127);
    header.u8(64);
    header.u8(0);
    header.u8(0);
    header.u32(0);
    header.u8(1);
    header.u8(127);
    header.u8(0);
    header.u8(0);
    header.u8(64);
    header.u8(0);
    header.u16(0);
    header.zeros(8);
    header.zeros(127 * 16);
    header.u8(64);
    header.u8(0);
    header.u8(127);
    header.u8(64);
    header.u8(60);
    header.u8(0);
    header.u8(0);
    header.u8(127);
    header.zeros(6);
    header.u8(0);
    header.u8(0);
    header.u16(adsr1);
    header.u16(adsr2);
    header.u16(0);
    header.u16(0);
    header.zeros(8);
    header.zeros(15 * 32);
    header.u16(static_cast<std::uint16_t>(vag_units));
    header.zeros(255 * 2);
    return header;
}

std::vector<std::uint8_t> adpcm_block(std::uint8_t flags, std::uint8_t packed) {
    std::vector<std::uint8_t> block(16, 0);
    block[1] = flags;
    block[2] = packed;
    return block;
}

} // namespace

TEST_CASE("PAL capture matches map to their event cues while ambiguous rows stay unset") {
    REQUIRE(std::size(oscilline::kSfxMap) == static_cast<std::size_t>(oscilline::SfxId::Count));
    for (std::size_t i = 0; i < std::size(oscilline::kSfxMap); ++i) {
        const oscilline::SfxMapEntry& entry = oscilline::kSfxMap[i];
        CHECK(entry.id == static_cast<oscilline::SfxId>(i));
        CHECK(entry.evidence != nullptr);
    }

    const auto& menu_select =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuSelect)];
    CHECK(menu_select.bank == oscilline::kSfxBankTitle);
    CHECK(menu_select.program == 0);
    CHECK(menu_select.tone == 1);
    CHECK(menu_select.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(menu_select.rate_confirmed);
    CHECK(menu_select.confidence == oscilline::SfxConfidence::High);

    const auto& menu_follow =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuSelectFollow)];
    CHECK(menu_follow.bank == oscilline::kSfxBankGame);
    CHECK(menu_follow.program == 2);
    CHECK(menu_follow.tone == 0);
    CHECK(menu_follow.rate_hz == 11025);
    CHECK(menu_follow.rate_confirmed);
    CHECK(menu_follow.confidence == oscilline::SfxConfidence::High);

    const auto& menu_loop =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuLoop)];
    CHECK(menu_loop.bank == oscilline::kSfxBankGame);
    CHECK(menu_loop.program == 2);
    CHECK(menu_loop.tone == 0);
    CHECK(menu_loop.rate_hz == 11025);
    CHECK(menu_loop.rate_confirmed);
    CHECK(menu_loop.loop);

    for (int kind = 0; kind < oscilline::kObstacleKindCount; ++kind) {
        const auto& clear =
            oscilline::kSfxMap[static_cast<std::size_t>(oscilline::sfx_cleared(kind))];
        CHECK(clear.bank == oscilline::kSfxBankGame);
        CHECK(clear.program == 1);
        CHECK(clear.tone == kind);
        CHECK(clear.rate_hz == 11025);
        CHECK(clear.rate_confirmed);
        CHECK(clear.confidence == oscilline::SfxConfidence::High);
        CHECK_FALSE(clear.loop);
        const auto& miss =
            oscilline::kSfxMap[static_cast<std::size_t>(oscilline::sfx_missed(kind))];
        CHECK(miss.bank == oscilline::kSfxBankGame);
        CHECK(miss.program == 1);
        CHECK(miss.tone == 10);
        CHECK(miss.rate_hz == 11025);
        CHECK(miss.rate_confirmed);
        CHECK(miss.confidence == oscilline::SfxConfidence::High);
    }

    const auto& form_change =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::FormChange)];
    CHECK(form_change.bank == oscilline::kSfxBankNone);
    CHECK(form_change.confidence == oscilline::SfxConfidence::Low);
    const auto& promote =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::SuperPromote)];
    CHECK(promote.bank == oscilline::kSfxBankGame);
    CHECK(promote.program == 1);
    CHECK(promote.tone == 14);
    CHECK(promote.vag == 19);
    CHECK(promote.rate_hz == 11025);
    CHECK(promote.rate_confirmed);
    CHECK_FALSE(promote.loop);
    CHECK(promote.confidence == oscilline::SfxConfidence::High);
    CHECK(oscilline::kSuperTransformSfxId == static_cast<int>(oscilline::SfxId::SuperPromote));
    const auto& menu_move =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuMove)];
    CHECK(menu_move.bank == oscilline::kSfxBankTitle);
    CHECK(menu_move.program == 0);
    CHECK(menu_move.tone == 6);
    CHECK(menu_move.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(menu_move.rate_confirmed);
    CHECK(menu_move.confidence == oscilline::SfxConfidence::High);
    CHECK(menu_move.vag == oscilline::kSfxVagUnset);

    const auto& menu_back =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuBack)];
    CHECK(menu_back.bank == oscilline::kSfxBankGame);
    CHECK(menu_back.vag == 20);
    CHECK(menu_back.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(menu_back.rate_confirmed);
    CHECK(menu_back.confidence == oscilline::SfxConfidence::High);

    const auto& round_start =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::RoundStart)];
    CHECK(round_start.bank == oscilline::kSfxBankGame);
    CHECK(round_start.program == 0);
    CHECK(round_start.tone == 11);
    CHECK(round_start.rate_hz == 16000);
    CHECK(round_start.rate_confirmed);

    const auto& game_over =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::GameOver)];
    CHECK(game_over.bank == oscilline::kSfxBankGame);
    CHECK(game_over.program == 0);
    CHECK(game_over.tone == 12);
    CHECK(game_over.rate_hz == 16000);
    CHECK(game_over.rate_hz == round_start.rate_hz);
    CHECK_FALSE(game_over.rate_confirmed);

    const auto& level_complete =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::LevelComplete)];
    CHECK(level_complete.bank == oscilline::kSfxBankGame);
    CHECK(level_complete.program == 0);
    CHECK(level_complete.tone == 13);
    CHECK(level_complete.rate_hz == 16000);
    CHECK(level_complete.rate_confirmed);
}

TEST_CASE("a linear attack steps by the documented amount") {
    // Shift 11, step index 0 (+7), linear. cycles = 1, amount = 7.
    oscilline::AdsrState state;
    oscilline::adsr_key_on(state, static_cast<std::uint16_t>(11 << 10), 0);
    int level = 0;
    for (int i = 0; i < 10; ++i) {
        level = oscilline::adsr_tick(state);
    }
    CHECK(level == 70);

    // Shift 0, step +7. amount = 7 << 11 = 14336, so the envelope fills in three steps.
    oscilline::adsr_key_on(state, 0, 0);
    CHECK(oscilline::adsr_tick(state) == 14336);
    CHECK(oscilline::adsr_tick(state) == 28672);
    CHECK(oscilline::adsr_tick(state) == 32767);
    // Decay is exponential with step -8. 32767 + (-8 << 11) * 32767 / 32768 = 16384.
    CHECK(oscilline::adsr_tick(state) == 16384);
}

TEST_CASE("resample length at 11025 Hz is four output frames per source sample") {
    const auto block = adpcm_block(0, 0x01);
    auto decoded = oscilline::decode_spu_adpcm(block);
    REQUIRE(decoded);
    REQUIRE(decoded.value().samples.size() == 28);
    CHECK(decoded.value().samples[0] == 4096);
    CHECK(decoded.value().samples[1] == 0);

    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    oscilline::SfxNote note;
    note.pcm = decoded.value().samples.data();
    note.sample_count = 28;
    note.rate_hz = 11025;
    note.use_adsr = false;
    note.gain_l_q15 = 32767;
    note.gain_r_q15 = 0;
    mixer.post_note(note);

    std::vector<std::int16_t> first(111 * 2, 0);
    mixer.mix(first, 111);
    CHECK(mixer.active_voices() == 1);
    CHECK(first[0] == 4096);
    CHECK(first[2] == 3072);
    CHECK(first[4] == 2048);
    CHECK(first[6] == 1024);

    std::vector<std::int16_t> rest(2, 0);
    mixer.mix(rest, 1);
    CHECK(mixer.active_voices() == 0);
}

TEST_CASE("adpcm loop flags repeat and a one-shot end stops") {
    std::vector<std::uint8_t> body = adpcm_block(0x04, 0x01);
    const auto tail = adpcm_block(0x03, 0x10);
    body.insert(body.end(), tail.begin(), tail.end());
    const Buf header = tone_header(0, 0, 4);
    auto bank = oscilline::parse_vab(header.bytes, body);
    REQUIRE(bank);
    const oscilline::DecodedVag& vag = bank.value().vags[0];
    CHECK(vag.pcm.size() == 56);
    CHECK(vag.loop.has_start);
    CHECK(vag.loop.has_end);
    CHECK(vag.loop.repeat);
    CHECK(vag.loop.start_sample == 0);
    CHECK(vag.loop.end_sample == 56);
    CHECK(vag.pcm[0] != vag.pcm[28]);

    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    oscilline::SfxNote note;
    note.pcm = vag.pcm.data();
    note.sample_count = static_cast<int>(vag.pcm.size());
    note.rate_hz = oscilline::kCddaRate;
    note.loop = true;
    note.loop_start = vag.loop.start_sample;
    note.loop_end = vag.loop.end_sample;
    note.use_adsr = false;
    note.gain_l_q15 = 32767;
    note.gain_r_q15 = 0;
    mixer.post_note(note);
    std::vector<std::int16_t> looped(120 * 2, 0);
    mixer.mix(looped, 120);
    CHECK(mixer.active_voices() == 1);
    CHECK(looped[0] == vag.pcm[0]);
    CHECK(looped[28 * 2] == vag.pcm[28]);
    CHECK(looped[56 * 2] == vag.pcm[0]);
    CHECK(looped[84 * 2] == vag.pcm[28]);

    std::vector<std::uint8_t> once = adpcm_block(0x01, 0x01);
    auto shot = oscilline::decode_spu_adpcm(once);
    REQUIRE(shot);
    CHECK(shot.value().loop.has_end);
    CHECK_FALSE(shot.value().loop.repeat);
    auto stop_owned = make_mixer();
    auto& stop_mix = *stop_owned;
    oscilline::SfxNote stop;
    stop.pcm = shot.value().samples.data();
    stop.sample_count = static_cast<int>(shot.value().samples.size());
    stop.rate_hz = oscilline::kCddaRate;
    stop.use_adsr = false;
    stop.gain_l_q15 = 32767;
    stop_mix.post_note(stop);
    std::vector<std::int16_t> head(27 * 2, 0);
    stop_mix.mix(head, 27);
    CHECK(stop_mix.active_voices() == 1);
    std::vector<std::int16_t> last(2, 0);
    stop_mix.mix(last, 1);
    CHECK(stop_mix.active_voices() == 0);
}

TEST_CASE("the mixer saturates and steals the quietest voice") {
    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    const std::int16_t hot = 32767;
    const std::int16_t cold = -32768;
    oscilline::SfxNote left;
    left.pcm = &hot;
    left.sample_count = 1;
    left.rate_hz = oscilline::kCddaRate;
    left.use_adsr = false;
    left.gain_l_q15 = 32767;
    left.gain_r_q15 = 32767;
    oscilline::SfxNote right = left;
    right.pcm = &cold;
    mixer.post_note(left);
    mixer.post_note(left);
    std::vector<std::int16_t> clipped(2, 0);
    mixer.mix(clipped, 1);
    CHECK(clipped[0] == 32767);
    CHECK(clipped[1] == 32767);

    auto negative_owned = make_mixer();
    auto& negative = *negative_owned;
    negative.post_note(right);
    negative.post_note(right);
    std::vector<std::int16_t> floor(2, 0);
    negative.mix(floor, 1);
    CHECK(floor[0] == -32768);
    CHECK(floor[1] == -32768);

    std::vector<std::int16_t> loop_pcm(8, 1000);
    auto crowd_owned = make_mixer();
    auto& crowd = *crowd_owned;
    for (int i = 0; i < oscilline::kSfxVoiceLimit; ++i) {
        oscilline::SfxNote note;
        note.pcm = loop_pcm.data();
        note.sample_count = static_cast<int>(loop_pcm.size());
        note.rate_hz = oscilline::kCddaRate;
        note.loop = true;
        note.loop_start = 0;
        note.loop_end = static_cast<int>(loop_pcm.size());
        note.use_adsr = false;
        note.gain_l_q15 = 1000 * (i + 1);
        note.gain_r_q15 = note.gain_l_q15;
        note.tag = static_cast<std::uint32_t>(i + 1);
        crowd.post_note(note);
    }
    std::vector<std::int16_t> keep(16, 0);
    crowd.mix(keep, 8);
    CHECK(crowd.active_voices() == oscilline::kSfxVoiceLimit);
    oscilline::SfxNote extra = {};
    extra.pcm = loop_pcm.data();
    extra.sample_count = static_cast<int>(loop_pcm.size());
    extra.rate_hz = oscilline::kCddaRate;
    extra.loop = true;
    extra.loop_end = static_cast<int>(loop_pcm.size());
    extra.use_adsr = false;
    extra.gain_l_q15 = 30000;
    extra.gain_r_q15 = 30000;
    extra.tag = 100;
    crowd.post_note(extra);
    crowd.mix(keep, 8);
    CHECK(crowd.active_voices() == oscilline::kSfxVoiceLimit);
    CHECK_FALSE(crowd.voice_tag_active(1));
    CHECK(crowd.voice_tag_active(100));
    CHECK(crowd.voice_tag_active(2));
}

TEST_CASE("silence leaves music untouched and a voice ducks it") {
    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    std::vector<std::int16_t> music(16, 0);
    for (int i = 0; i < 8; ++i) {
        music[static_cast<std::size_t>(i) * 2u] = 1000;
        music[static_cast<std::size_t>(i) * 2u + 1u] = -1000;
    }
    const auto copy = music;
    mixer.mix(music, 8);
    CHECK(music == copy);
    CHECK(mixer.music_gain_q15() == 32767);

    std::vector<std::int16_t> zeros(4, 0);
    oscilline::SfxNote note;
    note.pcm = zeros.data();
    note.sample_count = static_cast<int>(zeros.size());
    note.rate_hz = oscilline::kCddaRate;
    note.loop = true;
    note.loop_end = static_cast<int>(zeros.size());
    note.use_adsr = false;
    note.gain_l_q15 = 32767;
    note.gain_r_q15 = 32767;
    mixer.post_note(note);
    std::vector<std::int16_t> ducked(40 * 2, 10000);
    mixer.mix(ducked, 40);
    CHECK(ducked[0] == 10000);
    // 22937 is 0.70 in Q15. 10000 * 22937 >> 15 is 6999.
    CHECK(ducked[30 * 2] == 6999);
    CHECK(mixer.music_gain_q15() == 22937);
}

TEST_CASE("a low-confidence row stays on the placeholder") {
    const auto block = adpcm_block(0, 0x01);
    const Buf header = tone_header(0, 0, 2);
    auto parsed = oscilline::parse_vab(header.bytes, block);
    REQUIRE(parsed);
    oscilline::SfxLibrary library;
    library.banks[oscilline::kSfxBankGame].loaded = true;
    library.banks[oscilline::kSfxBankGame].vab = std::move(parsed.value());

    oscilline::SfxMapEntry row;
    row.id = oscilline::SfxId::MenuMove;
    row.bank = oscilline::kSfxBankGame;
    row.program = 0;
    row.tone = 0;
    row.rate_hz = oscilline::kCddaRate;
    row.confidence = oscilline::SfxConfidence::Low;
    row.evidence = "format; synthetic row for the confidence gate";

    oscilline::SfxConfig plain;
    oscilline::SfxConfig experimental;
    experimental.experimental = true;
    const auto map = std::span<const oscilline::SfxMapEntry>(&row, 1);
    oscilline::SfxMixer held(&library, map, plain);
    oscilline::SfxMixer opened(&library, map, experimental);
    oscilline::SfxMixer placeholders(nullptr, map, plain);

    CHECK(held.resolve(oscilline::SfxId::MenuMove).placeholder);
    CHECK_FALSE(opened.resolve(oscilline::SfxId::MenuMove).placeholder);

    held.post(oscilline::SfxId::MenuMove);
    placeholders.post(oscilline::SfxId::MenuMove);
    opened.post(oscilline::SfxId::MenuMove);
    std::vector<std::int16_t> a(200 * 2, 0);
    std::vector<std::int16_t> b(200 * 2, 0);
    std::vector<std::int16_t> c(200 * 2, 0);
    held.mix(a, 200);
    placeholders.mix(b, 200);
    opened.mix(c, 200);
    CHECK(a == b);
    CHECK(a != c);

    row.confidence = oscilline::SfxConfidence::Medium;
    oscilline::SfxMixer candidate_held(&library, map, plain);
    oscilline::SfxMixer candidate_opened(&library, map, experimental);
    CHECK(candidate_held.resolve(oscilline::SfxId::MenuMove).placeholder);
    CHECK_FALSE(candidate_opened.resolve(oscilline::SfxId::MenuMove).placeholder);

    row.confidence = oscilline::SfxConfidence::High;
    oscilline::SfxMixer confirmed(&library, map, plain);
    CHECK_FALSE(confirmed.resolve(oscilline::SfxId::MenuMove).placeholder);
}

TEST_CASE("imported one-shots fade to silence at their end") {
    auto mixer = make_mixer();
    std::vector<std::int16_t> pcm(1000, 12000);
    oscilline::SfxNote note;
    note.pcm = pcm.data();
    note.sample_count = static_cast<int>(pcm.size());
    note.rate_hz = oscilline::kCddaRate;
    note.use_adsr = false;
    note.fade_at_end = true;
    note.gain_l_q15 = 32767;
    note.gain_r_q15 = 32767;
    mixer->post_note(note);

    std::vector<std::int16_t> output(pcm.size() * 2u, 0);
    mixer->mix(output, static_cast<int>(pcm.size()));
    CHECK(output[0] == 11999);
    CHECK(output[(1000 - oscilline::kCddaRate / 200 - 1) * 2] == 11999);
    CHECK(output[(1000 - 1) * 2] < 100);
    CHECK(mixer->active_voices() == 0);
}

TEST_CASE("gaussian interpolation of a constant is that constant") {
    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    std::vector<std::int16_t> pcm(16, 1000);
    oscilline::SfxNote note;
    note.pcm = pcm.data();
    note.sample_count = static_cast<int>(pcm.size());
    note.rate_hz = 22050;
    note.use_adsr = false;
    note.resample = oscilline::SfxResample::Gaussian4;
    note.gain_l_q15 = 32767;
    note.gain_r_q15 = 0;
    mixer.post_note(note);
    std::vector<std::int16_t> out(32 * 2, 0);
    mixer.mix(out, 32);
    for (int i = 0; i < 16; ++i) {
        CHECK(out[static_cast<std::size_t>(i) * 2u] == 1000);
    }
}

TEST_CASE("an offline mix of synthetic input has a stable hash") {
    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    const std::int16_t pcm[] = {1000, -1000, 2000, -2000, 1500, -1500, 500, -500};
    oscilline::SfxNote note;
    note.pcm = pcm;
    note.sample_count = 8;
    note.rate_hz = 11025;
    note.use_adsr = false;
    note.gain_l_q15 = 20000;
    note.gain_r_q15 = 10000;
    mixer.post_note(note);
    std::vector<std::int16_t> music(32 * 2, 0);
    for (int i = 0; i < 32; ++i) {
        music[static_cast<std::size_t>(i) * 2u] = 400;
        music[static_cast<std::size_t>(i) * 2u + 1u] = -200;
    }
    mixer.mix(music, 32);
    CHECK(fnv1a(music) == 17576395465526999852ull);
}

TEST_CASE("posts from another thread are mixed") {
    auto mixer_owned = make_mixer();
    auto& mixer = *mixer_owned;
    std::atomic<bool> run{false};
    std::thread producer([&] {
        while (!run.load()) {
        }
        for (int i = 0; i < 30; ++i) {
            mixer.post(oscilline::SfxId::MenuSelect);
        }
    });
    std::thread consumer([&] {
        while (!run.load()) {
        }
        std::vector<std::int16_t> buffer(64 * 2, 0);
        for (int i = 0; i < 30; ++i) {
            mixer.mix(buffer, 64);
        }
    });
    run.store(true);
    producer.join();
    consumer.join();
    std::vector<std::int16_t> tail(64 * 2, 0);
    mixer.mix(tail, 64);
    CHECK_FALSE(mixer.pending());
}

TEST_CASE("game audio is loaded from the audio folder and title is not required") {
    using oscilline::testutil::IsoNode;
    const auto block = adpcm_block(0, 0x01);
    const Buf header = tone_header(0, 0, 2);
    IsoNode root;
    root.directory = true;
    root.xa_attributes = 0x8D55;
    IsoNode game;
    game.name = "GAME";
    game.directory = true;
    game.xa_attributes = 0x8D55;
    IsoNode audio;
    audio.name = "AUDIO";
    audio.directory = true;
    audio.xa_attributes = 0x8D55;
    IsoNode vh;
    vh.name = "BANK.VH";
    vh.data = header.bytes;
    vh.xa_attributes = 0x0D55;
    IsoNode vb;
    vb.name = "BANK.VB";
    vb.data = block;
    vb.xa_attributes = 0x0D55;
    audio.children.push_back(std::move(vh));
    audio.children.push_back(std::move(vb));
    game.children.push_back(std::move(audio));
    root.children.push_back(std::move(game));

    auto image =
        oscilline::DiscImage::open_iso_bytes(oscilline::testutil::build_iso(root, "OSCILLINE"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    const oscilline::SfxLibrary library = oscilline::load_sfx_library(volume.value());
    CHECK(library.banks[oscilline::kSfxBankGame].loaded);
    CHECK(library.banks[oscilline::kSfxBankGame].vab.vags[0].pcm.size() == 28);
    CHECK_FALSE(library.banks[oscilline::kSfxBankTitle].loaded);
    CHECK_FALSE(library.banks[oscilline::kSfxBankTutorial].loaded);
}

TEST_CASE("a clear and a miss are reported without changing the judgment") {
    oscilline::CourseTimeline course;
    course.duration_ms = 5000;
    oscilline::CourseEvent event;
    event.obstacle = 2;
    event.hit_ms = 1000;
    course.events.push_back(event);

    oscilline::PlayState cleared;
    std::vector<oscilline::PlayHit> hits;
    oscilline::play_advance(cleared, course, 0, 1000, oscilline::kActionLoop, &hits);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].obstacle == 2);
    CHECK(hits[0].judgment == oscilline::Judgment::Perfect);
    CHECK_FALSE(hits[0].form_changed);
    CHECK(cleared.perfects == 1);

    oscilline::PlayState missed;
    hits.clear();
    const oscilline::ObstacleWindow miss_window = oscilline::obstacle_window(event);
    oscilline::play_advance(missed, course, 0, miss_window.good_close + 1, 0, &hits);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].judgment == oscilline::Judgment::Miss);
    CHECK(missed.misses == 1);
    CHECK_FALSE(hits[0].form_changed);
}

void fill_tone_bank(oscilline::VabBank& vab, int programs) {
    vab.header.program_count = static_cast<std::uint16_t>(programs);
    vab.header.master_volume = 127;
    vab.header.programs.assign(static_cast<std::size_t>(programs), oscilline::VabProgram{});
    for (oscilline::VabProgram& program : vab.header.programs) {
        program.volume = 127;
        program.tone_count = 16;
    }
    vab.header.tones.assign(static_cast<std::size_t>(programs) * 16u, oscilline::VabTone{});
    vab.vags.assign(16, oscilline::DecodedVag{});
    for (int vag = 0; vag < 16; ++vag) {
        vab.vags[static_cast<std::size_t>(vag)].pcm = {static_cast<std::int16_t>(1000 + vag * 10)};
    }
    for (int program = 0; program < programs; ++program) {
        for (int tone = 0; tone < 16; ++tone) {
            oscilline::VabTone& slot = vab.header.tones[static_cast<std::size_t>(program) * 16u +
                                                        static_cast<std::size_t>(tone)];
            slot.vag = static_cast<std::uint16_t>(tone);
            slot.volume = 127;
            slot.pan = 64;
            slot.priority = 64;
        }
    }
}

TEST_CASE("high follow-up rows resolve by default and medium rows stay gated") {
    oscilline::SfxLibrary library;
    library.banks[oscilline::kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankGame].vab, 3);
    library.banks[oscilline::kSfxBankGame].vab.vags.resize(84);
    library.banks[oscilline::kSfxBankGame].vab.vags[20].pcm = {2000};
    library.banks[oscilline::kSfxBankGame].vab.vags[83].pcm = {3000};
    library.banks[oscilline::kSfxBankTitle].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankTitle].vab, 7);

    const auto plain = make_mixer(&library, false);
    const auto opened = make_mixer(&library, true);

    const oscilline::SfxChoice whiff = plain->resolve(oscilline::SfxId::Whiff);
    CHECK_FALSE(whiff.placeholder);
    CHECK(whiff.bank == oscilline::kSfxBankGame);
    CHECK(whiff.program == 1);
    CHECK(whiff.tone == 11);
    CHECK(whiff.rate_hz == 11025);
    CHECK(whiff.confidence == oscilline::SfxConfidence::High);

    const oscilline::SfxChoice alt = plain->resolve(oscilline::SfxId::WhiffAlt);
    CHECK(alt.placeholder);
    CHECK(alt.bank == oscilline::kSfxBankGame);
    CHECK(alt.program == 1);
    CHECK(alt.tone == 15);
    CHECK(alt.rate_hz == 12371);
    CHECK(alt.confidence == oscilline::SfxConfidence::Medium);
    CHECK_FALSE(opened->resolve(oscilline::SfxId::WhiffAlt).placeholder);

    const oscilline::SfxChoice crash = plain->resolve(oscilline::SfxId::PostCrashPress);
    CHECK(crash.placeholder);
    CHECK(crash.program == 1);
    CHECK(crash.tone == 12);
    CHECK(crash.rate_hz == 12371);
    CHECK_FALSE(opened->resolve(oscilline::SfxId::PostCrashPress).placeholder);

    const oscilline::SfxChoice select = plain->resolve(oscilline::SfxId::MenuSelect);
    CHECK_FALSE(select.placeholder);
    CHECK(select.bank == oscilline::kSfxBankTitle);
    CHECK(select.program == 0);
    CHECK(select.tone == 1);
    const oscilline::SfxChoice follow = plain->resolve(oscilline::SfxId::MenuSelectFollow);
    CHECK_FALSE(follow.placeholder);
    CHECK(follow.bank == oscilline::kSfxBankGame);
    CHECK(follow.program == 2);
    CHECK(follow.tone == 0);
    CHECK(follow.rate_hz == 11025);

    const oscilline::SfxChoice confirm = plain->resolve(oscilline::SfxId::DifficultyConfirm);
    CHECK_FALSE(confirm.placeholder);
    CHECK(confirm.bank == oscilline::kSfxBankTitle);
    CHECK(confirm.tone == 1);
    CHECK(confirm.rate_hz == oscilline::kSfxPitch9991Hz);
    const oscilline::SfxChoice confirm_b =
        plain->resolve(oscilline::SfxId::DifficultyConfirmFollow);
    CHECK(confirm_b.placeholder);
    CHECK(confirm_b.bank == oscilline::kSfxBankGame);
    CHECK(confirm_b.vag == 83);
    CHECK(confirm_b.confidence == oscilline::SfxConfidence::Medium);
    CHECK_FALSE(opened->resolve(oscilline::SfxId::DifficultyConfirmFollow).placeholder);

    const oscilline::SfxChoice back = plain->resolve(oscilline::SfxId::MenuBack);
    CHECK_FALSE(back.placeholder);
    CHECK(back.bank == oscilline::kSfxBankGame);
    CHECK(back.vag == 20);
    CHECK(back.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(back.confidence == oscilline::SfxConfidence::High);

    const oscilline::SfxChoice cursor = plain->resolve(oscilline::SfxId::MenuMove);
    CHECK_FALSE(cursor.placeholder);
    CHECK(cursor.bank == oscilline::kSfxBankTitle);
    CHECK(cursor.tone == 6);
    CHECK(cursor.rate_hz == oscilline::kSfxPitch9991Hz);

    const oscilline::SfxChoice loop = plain->resolve(oscilline::sfx_cleared(2));
    CHECK_FALSE(loop.placeholder);
    CHECK(loop.bank == oscilline::kSfxBankGame);
    CHECK(loop.program == 1);
    CHECK(loop.tone == 2);
    CHECK(loop.rate_hz == 11025);
    const oscilline::SfxChoice wave = plain->resolve(oscilline::sfx_cleared(9));
    CHECK_FALSE(wave.placeholder);
    CHECK(wave.tone == 9);
    CHECK(wave.rate_hz == 11025);
    CHECK(plain->resolve(oscilline::SfxId::RoundStart).rate_hz == 16000);
    CHECK_FALSE(plain->resolve(oscilline::SfxId::RoundStart).placeholder);
    CHECK(plain->resolve(oscilline::SfxId::LevelComplete).rate_hz == 16000);
    CHECK_FALSE(plain->resolve(oscilline::SfxId::LevelComplete).placeholder);
    CHECK(plain->resolve(oscilline::SfxId::GameOver).rate_hz == 16000);
    CHECK_FALSE(plain->resolve(oscilline::SfxId::GameOver).rate_confirmed);
    CHECK_FALSE(plain->resolve(oscilline::SfxId::GameOver).placeholder);
    const oscilline::SfxChoice miss = plain->resolve(oscilline::sfx_missed(3));
    CHECK_FALSE(miss.placeholder);
    CHECK(miss.tone == 10);
    CHECK(miss.rate_hz == 11025);

    plain->post(oscilline::SfxId::Whiff);
    const auto stand_in = make_mixer(nullptr, false);
    stand_in->post(oscilline::SfxId::Whiff);
    std::vector<std::int16_t> sample(80 * 2, 0);
    std::vector<std::int16_t> placeholder(80 * 2, 0);
    plain->mix(sample, 80);
    stand_in->mix(placeholder, 80);
    CHECK(sample != placeholder);

    const auto pair = make_mixer(&library, false);
    pair->post(oscilline::SfxId::MenuSelect);
    pair->post(oscilline::SfxId::MenuSelectFollow);
    std::vector<std::int16_t> both(2 * 2, 0);
    pair->mix(both, 2);
    CHECK(pair->active_voices() == 2);

    const auto wheel = make_mixer(&library, false);
    wheel->post(oscilline::SfxId::DifficultyConfirm);
    wheel->post(oscilline::SfxId::DifficultyConfirmFollow);
    wheel->mix(both, 2);
    CHECK(wheel->active_voices() == 2);
}

TEST_CASE("title CROSS and wheel CROSS choose different cue pairs") {
    const oscilline::MenuSfxCues title =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Main, oscilline::MenuAction::Confirm);
    CHECK(title.count == 1);
    CHECK(title.ids[0] == oscilline::SfxId::TitleConfirm);

    const oscilline::MenuSfxCues language =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Language, oscilline::MenuAction::Confirm);
    CHECK(language.count == 1);
    CHECK(language.ids[0] == oscilline::SfxId::TitleConfirm);

    const oscilline::MenuSfxCues music =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Music, oscilline::MenuAction::Confirm);
    CHECK(music.count == 2);
    CHECK(music.ids[0] == oscilline::SfxId::MenuSelect);
    CHECK(music.ids[1] == oscilline::SfxId::MenuSelectFollow);

    const oscilline::MenuSfxCues wheel =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Wheel, oscilline::MenuAction::Confirm);
    CHECK(wheel.count == 2);
    CHECK(wheel.ids[0] == oscilline::SfxId::DifficultyConfirm);
    CHECK(wheel.ids[1] == oscilline::SfxId::DifficultyConfirmFollow);

    // Escape plays what confirming the page's Back row plays.
    const oscilline::MenuSfxCues back =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Wheel, oscilline::MenuAction::Back);
    CHECK(back.count == 2);
    CHECK(back.ids[0] == oscilline::SfxId::DifficultyConfirm);
    CHECK(back.ids[1] == oscilline::SfxId::DifficultyConfirmFollow);
    const oscilline::MenuSfxCues language_back =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Language, oscilline::MenuAction::Back);
    CHECK(language_back.count == 1);
    CHECK(language_back.ids[0] == oscilline::SfxId::TitleConfirm);
    for (const oscilline::MenuPage page :
         {oscilline::MenuPage::Music, oscilline::MenuPage::About, oscilline::MenuPage::Scores}) {
        const oscilline::MenuSfxCues escape =
            oscilline::menu_sfx_cues(page, oscilline::MenuAction::Back);
        const oscilline::MenuSfxCues row =
            oscilline::menu_sfx_cues(page, oscilline::MenuAction::Confirm);
        REQUIRE(escape.count == row.count);
        for (int i = 0; i < row.count; ++i) {
            CHECK(escape.ids[i] == row.ids[i]);
        }
    }
    CHECK(oscilline::menu_sfx_cues(oscilline::MenuPage::Main, oscilline::MenuAction::Back).count ==
          0);

    const oscilline::MenuSfxCues move =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Main, oscilline::MenuAction::Move);
    CHECK(move.count == 1);
    CHECK(move.ids[0] == oscilline::SfxId::MenuMove);

    const oscilline::MenuSfxCues options_move =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Options, oscilline::MenuAction::Move);
    CHECK(options_move.count == 1);
    CHECK(options_move.ids[0] == oscilline::SfxId::MenuMove);
    const oscilline::MenuSfxCues options_adjust =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Options, oscilline::MenuAction::Adjust);
    CHECK(options_adjust.count == 1);
    CHECK(options_adjust.ids[0] == oscilline::SfxId::MenuMove);
    const oscilline::MenuSfxCues options_back =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Options, oscilline::MenuAction::Back);
    CHECK(options_back.count == 2);
    CHECK(options_back.ids[0] == oscilline::SfxId::MenuSelect);
    CHECK(options_back.ids[1] == oscilline::SfxId::MenuSelectFollow);

    const oscilline::MenuSfxCues idle =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Language, oscilline::MenuAction::None);
    CHECK(idle.count == 0);
}

TEST_CASE("an empty press is a whiff and does not change the judgment") {
    oscilline::CourseTimeline course;
    course.duration_ms = 8000;
    oscilline::CourseEvent first;
    first.obstacle = 0;
    first.hit_ms = 2000;
    oscilline::CourseEvent second;
    second.obstacle = 2;
    second.hit_ms = 5000;
    course.events.push_back(first);
    course.events.push_back(second);

    oscilline::PlayState early;
    const oscilline::PlayAdvanceResult gap =
        oscilline::play_advance(early, course, 0, 400, oscilline::kActionBlock);
    CHECK(gap.whiff);
    CHECK_FALSE(gap.post_crash_press);
    CHECK(early.score == 0);
    CHECK(early.misses == 0);
    CHECK(early.last == oscilline::Judgment::None);
    CHECK_FALSE(early.press_after_crash);

    oscilline::PlayState hit;
    const oscilline::PlayAdvanceResult clear =
        oscilline::play_advance(hit, course, 0, 2000, oscilline::kActionBlock);
    CHECK_FALSE(clear.whiff);
    CHECK_FALSE(clear.post_crash_press);
    CHECK(hit.perfects == 1);
    CHECK(hit.score == 3);

    oscilline::PlayState between = hit;
    const oscilline::PlayAdvanceResult empty =
        oscilline::play_advance(between, course, 2000, 2500, oscilline::kActionWave);
    CHECK(empty.whiff);
    CHECK_FALSE(empty.post_crash_press);
    CHECK(between.perfects == 1);
    CHECK(between.misses == 0);
    CHECK(between.event_index == 1);

    oscilline::PlayState missed;
    const oscilline::ObstacleWindow miss_window = oscilline::obstacle_window(first);
    const oscilline::PlayAdvanceResult timeout =
        oscilline::play_advance(missed, course, 0, miss_window.good_close + 1, 0);
    CHECK_FALSE(timeout.whiff);
    CHECK_FALSE(timeout.post_crash_press);
    CHECK(missed.misses == 1);
    CHECK(missed.score == 0);
    CHECK(missed.press_after_crash);

    oscilline::PlayState after = missed;
    const oscilline::PlayAdvanceResult next =
        oscilline::play_advance(after, course, 2200, 2600, oscilline::kActionPit);
    CHECK(next.whiff);
    CHECK(next.post_crash_press);
    CHECK(after.misses == 1);
    CHECK(after.score == 0);
    CHECK_FALSE(after.press_after_crash);

    oscilline::PlayState wrong;
    const oscilline::PlayAdvanceResult bad =
        oscilline::play_advance(wrong, course, 0, 2000, oscilline::kActionLoop);
    CHECK_FALSE(bad.whiff);
    CHECK_FALSE(bad.post_crash_press);
    CHECK(wrong.misses == 1);
    CHECK(wrong.last == oscilline::Judgment::Miss);
    CHECK(wrong.press_after_crash);

    oscilline::PlayState late;
    const oscilline::ObstacleWindow late_window = oscilline::obstacle_window(first);
    const oscilline::PlayAdvanceResult late_press = oscilline::play_advance(
        late, course, 0, late_window.good_close + 1, oscilline::kActionBlock);
    CHECK_FALSE(late_press.whiff);
    CHECK_FALSE(late_press.post_crash_press);
    CHECK(late.misses == 1);
    CHECK(late.press_after_crash);
}

static void expect_part(oscilline::SfxId id, int program, int tone) {
    const oscilline::SfxMapEntry& row = oscilline::kSfxMap[static_cast<std::size_t>(id)];
    CHECK(row.id == id);
    CHECK(row.bank == oscilline::kSfxBankGame);
    CHECK(row.program == program);
    CHECK(row.tone == tone);
    CHECK(row.vag == oscilline::kSfxVagUnset);
    CHECK(row.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(row.rate_confirmed);
    CHECK(row.confidence == oscilline::SfxConfidence::High);
    CHECK_FALSE(row.loop);
}

static std::uint32_t voice_tag(oscilline::SfxId id) {
    return static_cast<std::uint32_t>(id) + 1u;
}

TEST_CASE("wheel announcement rows and an edge move") {
    const oscilline::SfxMapEntry& title =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::TitleConfirm)];
    CHECK(title.bank == oscilline::kSfxBankTitle);
    CHECK(title.program == 0);
    CHECK(title.tone == 0);
    CHECK(title.rate_hz == oscilline::kSfxPitch9991Hz);
    CHECK(title.confidence == oscilline::SfxConfidence::High);

    const oscilline::SfxMapEntry& follow_b =
        oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::DifficultyConfirmFollow)];
    CHECK(follow_b.bank == oscilline::kSfxBankGame);
    CHECK(follow_b.vag == 83);
    CHECK(follow_b.confidence == oscilline::SfxConfidence::Medium);
    CHECK_FALSE(follow_b.rate_confirmed);

    expect_part(oscilline::SfxId::AnnounceDisc0, 15, 0);
    expect_part(oscilline::SfxId::AnnounceDisc1, 15, 1);
    expect_part(oscilline::SfxId::AnnounceJoin, 7, 14);
    expect_part(oscilline::SfxId::AnnounceCd0, 7, 13);
    expect_part(oscilline::SfxId::AnnounceCd1, 15, 3);
    expect_part(oscilline::SfxId::AnnounceOptions, 7, 7);
    expect_part(oscilline::SfxId::AnnounceBack, 6, 0);
    expect_part(oscilline::SfxId::AnnounceBronze, 7, 2);
    expect_part(oscilline::SfxId::AnnounceSilver, 7, 1);
    expect_part(oscilline::SfxId::AnnounceGold, 7, 0);
    expect_part(oscilline::SfxId::AnnounceScores, 8, 0);

    const auto disc = oscilline::game_wheel_announcement(oscilline::GameWheelItem::Disc);
    REQUIRE(disc.size() == 3);
    CHECK(disc[0] == oscilline::SfxId::AnnounceDisc0);
    CHECK(disc[1] == oscilline::SfxId::AnnounceDisc1);
    CHECK(disc[2] == oscilline::SfxId::AnnounceJoin);
    const auto own = oscilline::game_wheel_announcement(oscilline::GameWheelItem::OwnCd);
    REQUIRE(own.size() == 3);
    CHECK(own[0] == oscilline::SfxId::AnnounceCd0);
    CHECK(own[1] == oscilline::SfxId::AnnounceCd1);
    CHECK(own[2] == oscilline::SfxId::AnnounceJoin);
    CHECK(oscilline::game_wheel_announcement(oscilline::GameWheelItem::Options).size() == 1);
    CHECK(oscilline::game_wheel_announcement(oscilline::GameWheelItem::Options)[0] ==
          oscilline::SfxId::AnnounceOptions);
    CHECK(oscilline::game_wheel_announcement(oscilline::GameWheelItem::Back)[0] ==
          oscilline::SfxId::AnnounceBack);

    CHECK(oscilline::difficulty_wheel_announcement(0)[0] == oscilline::SfxId::AnnounceBronze);
    CHECK(oscilline::difficulty_wheel_announcement(1)[0] == oscilline::SfxId::AnnounceSilver);
    CHECK(oscilline::difficulty_wheel_announcement(2)[0] == oscilline::SfxId::AnnounceGold);
    CHECK(oscilline::difficulty_wheel_announcement(3)[0] == oscilline::SfxId::AnnounceScores);
    CHECK(oscilline::difficulty_wheel_announcement(4)[0] == oscilline::SfxId::AnnounceBack);

    oscilline::MenuClock clock;
    const oscilline::MenuStep edge =
        oscilline::menu_step(clock, oscilline::MenuInput{-1, false, false, 0}, {}, 0.f, false);
    CHECK(edge.action == oscilline::MenuAction::None);
    CHECK(clock.index == 0);
    CHECK(oscilline::menu_sfx_cues(oscilline::MenuPage::Main, edge.action).count == 0);
    CHECK(
        oscilline::menu_move_sequence(oscilline::MenuPage::Main, edge.action, clock.index).empty());

    const oscilline::MenuStep down =
        oscilline::menu_step(clock, oscilline::MenuInput{1, false, false, 0}, {}, 0.f, false);
    CHECK(down.action == oscilline::MenuAction::Move);
    const oscilline::MenuSfxCues blip =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Main, down.action);
    CHECK(blip.count == 1);
    CHECK(blip.ids[0] == oscilline::SfxId::MenuMove);
    CHECK(
        oscilline::menu_move_sequence(oscilline::MenuPage::Main, down.action, clock.index).empty());

    clock = {};
    clock.page = oscilline::MenuPage::Wheel;
    const oscilline::MenuStep turned =
        oscilline::menu_step(clock, oscilline::MenuInput{1, false, false}, {}, 0.f, false);
    CHECK(turned.action == oscilline::MenuAction::Move);
    CHECK(oscilline::menu_sfx_cues(oscilline::MenuPage::Wheel, turned.action).count == 0);
    const auto silver = oscilline::menu_move_sequence(
        oscilline::MenuPage::Wheel, turned.action, oscilline::menu_shown_index(clock));
    REQUIRE(silver.size() == 1);
    CHECK(silver[0] == oscilline::SfxId::AnnounceSilver);

    clock = {};
    clock.page = oscilline::MenuPage::Scores;
    const oscilline::MenuStep to_silver =
        oscilline::menu_step(clock, oscilline::MenuInput{1, false, false}, {}, 0.f, false);
    CHECK(to_silver.action == oscilline::MenuAction::Move);
    CHECK(clock.page == oscilline::MenuPage::Scores);
    CHECK(clock.index == 1);
    const auto silver_line =
        oscilline::menu_move_sequence(oscilline::MenuPage::Scores, to_silver.action, clock.index);
    REQUIRE(silver_line.size() == 1);
    CHECK(silver_line[0] == oscilline::SfxId::AnnounceSilver);

    clock.index = oscilline::kDifficultyCount;
    const oscilline::MenuStep to_gold =
        oscilline::menu_step(clock, oscilline::MenuInput{-1, false, false}, {}, 0.f, false);
    CHECK(to_gold.action == oscilline::MenuAction::Move);
    CHECK(clock.index == oscilline::kDifficultyCount - 1);
    const auto gold =
        oscilline::menu_move_sequence(oscilline::MenuPage::Scores, to_gold.action, clock.index);
    REQUIRE(gold.size() == 1);
    CHECK(gold[0] == oscilline::SfxId::AnnounceGold);

    const oscilline::MenuSfxCues back_cue =
        oscilline::menu_sfx_cues(oscilline::MenuPage::Scores, oscilline::MenuAction::Back);
    CHECK(back_cue.count == 2);
    CHECK(back_cue.ids[0] == oscilline::SfxId::MenuSelect);
}

TEST_CASE("announcement parts play in order and never together") {
    oscilline::SfxLibrary library;
    library.banks[oscilline::kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankGame].vab, 16);
    auto mixer = make_mixer(&library, false);
    const auto line = oscilline::game_wheel_announcement(oscilline::GameWheelItem::Disc);
    REQUIRE(line.size() == 3);
    mixer->post_sequence(line);
    std::vector<std::int16_t> buffer(2, 0);
    mixer->mix(buffer, 0);
    CHECK(mixer->active_voices() == 1);
    CHECK(mixer->voice_tag_active(voice_tag(line[0])));
    CHECK_FALSE(mixer->voice_tag_active(voice_tag(line[1])));
    CHECK_FALSE(mixer->voice_tag_active(voice_tag(line[2])));

    int part = 0;
    bool finished = false;
    for (int frame = 0; frame < 80; ++frame) {
        mixer->mix(buffer, 1);
        CHECK(mixer->active_voices() <= 1);
        int heard = 0;
        for (int i = 0; i < static_cast<int>(line.size()); ++i) {
            if (!mixer->voice_tag_active(voice_tag(line[static_cast<std::size_t>(i)]))) {
                continue;
            }
            ++heard;
            CHECK(i >= part);
            part = i;
        }
        CHECK(heard <= 1);
        if (part == static_cast<int>(line.size()) - 1 && mixer->active_voices() == 0) {
            finished = true;
            break;
        }
    }
    CHECK(finished);
    CHECK(part == 2);

    mixer->post_sequence(oscilline::difficulty_wheel_announcement(0));
    mixer->mix(buffer, 0);
    CHECK(mixer->voice_tag_active(voice_tag(oscilline::SfxId::AnnounceBronze)));
    mixer->post_sequence(oscilline::difficulty_wheel_announcement(1));
    mixer->mix(buffer, 0);
    CHECK_FALSE(mixer->voice_tag_active(voice_tag(oscilline::SfxId::AnnounceBronze)));
    CHECK(mixer->voice_tag_active(voice_tag(oscilline::SfxId::AnnounceSilver)));
    CHECK(mixer->active_voices() == 1);
}

TEST_CASE("a VAG index resolves without a program and tone") {
    oscilline::SfxLibrary library;
    library.banks[oscilline::kSfxBankGame].loaded = true;
    oscilline::VabBank& vab = library.banks[oscilline::kSfxBankGame].vab;
    vab.header.master_volume = 127;
    vab.vags.assign(84, oscilline::DecodedVag{});
    vab.vags[20].pcm = {1500, -1500};
    vab.vags[83].pcm = {800, -800};
    const auto plain = make_mixer(&library, false);
    const auto opened = make_mixer(&library, true);
    const oscilline::SfxChoice back = plain->resolve(oscilline::SfxId::MenuBack);
    CHECK_FALSE(back.placeholder);
    CHECK(back.vag == 20);
    const oscilline::SfxChoice gated = plain->resolve(oscilline::SfxId::DifficultyConfirmFollow);
    CHECK(gated.placeholder);
    CHECK(gated.vag == 83);
    CHECK_FALSE(opened->resolve(oscilline::SfxId::DifficultyConfirmFollow).placeholder);

    plain->post(oscilline::SfxId::MenuBack);
    std::vector<std::int16_t> sample(8, 0);
    plain->mix(sample, 4);
    CHECK(plain->active_voices() == 1);
    CHECK(sample != std::vector<std::int16_t>(8, 0));
}

TEST_CASE("the game bank prefers PSJ_SE when another pair sorts first") {
    using oscilline::testutil::IsoNode;
    const auto block = adpcm_block(0, 0x01);
    const Buf header = tone_header(0, 0, 2);
    IsoNode root;
    root.directory = true;
    root.xa_attributes = 0x8D55;
    IsoNode game;
    game.name = "GAME";
    game.directory = true;
    game.xa_attributes = 0x8D55;
    IsoNode audio;
    audio.name = "AUDIO";
    audio.directory = true;
    audio.xa_attributes = 0x8D55;
    IsoNode other_vh;
    other_vh.name = "AAA.VH";
    other_vh.data = header.bytes;
    other_vh.xa_attributes = 0x0D55;
    IsoNode other_vb;
    other_vb.name = "AAA.VB";
    other_vb.data = block;
    other_vb.xa_attributes = 0x0D55;
    IsoNode se_vh;
    se_vh.name = "PSJ_SE.VH";
    se_vh.data = header.bytes;
    se_vh.xa_attributes = 0x0D55;
    IsoNode se_vb;
    se_vb.name = "PSJ_SE.VB";
    se_vb.data = block;
    se_vb.xa_attributes = 0x0D55;
    audio.children.push_back(std::move(other_vh));
    audio.children.push_back(std::move(other_vb));
    audio.children.push_back(std::move(se_vh));
    audio.children.push_back(std::move(se_vb));
    game.children.push_back(std::move(audio));
    root.children.push_back(std::move(game));

    auto image =
        oscilline::DiscImage::open_iso_bytes(oscilline::testutil::build_iso(root, "OSCILLINE"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    const oscilline::SfxLibrary library = oscilline::load_sfx_library(volume.value());
    CHECK(library.banks[oscilline::kSfxBankGame].loaded);
    CHECK(library.banks[oscilline::kSfxBankGame].path.find("PSJ_SE") != std::string::npos);
}

TEST_CASE("the super transformation fires its cue once and skips the scribble burst") {
    using namespace oscilline;
    CourseTimeline timeline;
    timeline.duration_ms = 60000;
    for (int i = 0; i < kClearsToRise; ++i) {
        CourseEvent event;
        event.obstacle = 0;
        event.hit_ms = 1000 + i * 500;
        event.approach_ms = 400;
        event.scroll_approach_ms = 400;
        timeline.events.push_back(event);
    }

    PlayState state;
    state.form = Form::Rabbit;
    int promotions = 0;
    std::vector<PlayHit> promoting;
    for (int i = 0; i < kClearsToRise; ++i) {
        const ObstacleWindow window = obstacle_window(timeline.events[static_cast<std::size_t>(i)]);
        const std::int64_t when = (window.perfect_open + window.perfect_close) / 2;
        std::vector<PlayHit> hits;
        play_advance(state, timeline, when - 10, when, kActionBlock, &hits);
        for (const PlayHit& hit_row : hits) {
            if (hit_row.super_transform) {
                ++promotions;
                promoting.push_back(hit_row);
            }
        }
    }
    CHECK(state.form == Form::Super);
    CHECK(state.clear_run == 0);
    CHECK(state.burst_until_ms == 0);
    CHECK(promotions == 1);
    REQUIRE(promoting.size() == 1);
    CHECK(promoting[0].form_changed);
    CHECK(promoting[0].judgment == Judgment::Perfect);
    CHECK(promoting[0].obstacle == 0);

    SfxLibrary library;
    library.banks[kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[kSfxBankGame].vab, 3);
    library.banks[kSfxBankGame].vab.vags.resize(20);
    library.banks[kSfxBankGame].vab.vags[0].pcm.assign(400, 800);
    library.banks[kSfxBankGame].vab.vags[19].pcm.assign(400, 1000);

    SfxStage with_disc(false);
    with_disc.mixer().set_library(&library);
    post_play_hits(with_disc, promoting);
    std::vector<std::int16_t> heard(8 * 2, 0);
    with_disc.mixer().mix(heard, 8);
    const auto promote_tag = static_cast<std::uint32_t>(SfxId::SuperPromote) + 1u;
    const auto clear_tag = static_cast<std::uint32_t>(sfx_cleared(0)) + 1u;
    const auto form_tag = static_cast<std::uint32_t>(SfxId::FormChange) + 1u;
    CHECK(with_disc.mixer().active_voices() == 2);
    CHECK(with_disc.mixer().voice_tag_active(promote_tag));
    CHECK(with_disc.mixer().voice_tag_active(clear_tag));
    CHECK_FALSE(with_disc.mixer().voice_tag_active(form_tag));

    SfxStage no_disc(false);
    post_play_hits(no_disc, promoting);
    std::vector<std::int16_t> silent(8 * 2, 0);
    no_disc.mixer().mix(silent, 8);
    CHECK(no_disc.mixer().active_voices() == 1);
    CHECK_FALSE(no_disc.mixer().voice_tag_active(promote_tag));
    CHECK(no_disc.mixer().voice_tag_active(clear_tag));
    CHECK_FALSE(no_disc.mixer().voice_tag_active(form_tag));

    auto unducked = make_mixer(&library, false);
    unducked->post(SfxId::SuperPromote);
    std::vector<std::int16_t> music(40 * 2, 10000);
    unducked->mix(music, 40);
    CHECK(unducked->music_gain_q15() == 32767);
    CHECK(unducked->voice_tag_active(promote_tag));
}

TEST_CASE("the menu loop replaces itself and select-follow does not stack it") {
    using namespace oscilline;
    SfxLibrary library;
    library.banks[kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[kSfxBankGame].vab, 3);
    DecodedVag& bed = library.banks[kSfxBankGame].vab.vags[0];
    bed.pcm.assign(200, 1200);
    bed.loop.has_end = true;
    bed.loop.repeat = true;
    bed.loop.start_sample = 0;
    bed.loop.end_sample = static_cast<int>(bed.pcm.size());

    auto mixer = make_mixer(&library, false);
    const auto loop_tag = static_cast<std::uint32_t>(SfxId::MenuLoop) + 1u;
    const auto follow_tag = static_cast<std::uint32_t>(SfxId::MenuSelectFollow) + 1u;
    mixer->post(SfxId::MenuLoop);
    mixer->post(SfxId::MenuLoop);
    std::vector<std::int16_t> heard(32 * 2, 0);
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);
    CHECK(mixer->active_voices() == 1);

    mixer->post(SfxId::MenuSelectFollow);
    mixer->mix(heard, 32);
    CHECK_FALSE(mixer->voice_tag_active(follow_tag));
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    CHECK_FALSE(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    mixer->stop(SfxId::MenuLoop);
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 0);
    CHECK(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);
    CHECK_FALSE(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    auto plain = make_mixer();
    CHECK(plain->resolve(SfxId::MenuLoop).placeholder);
    CHECK_FALSE(ensure_menu_loop(*plain));
    plain->mix(heard, 32);
    CHECK(plain->active_with_tag(loop_tag) == 0);
    CHECK(plain->active_voices() == 0);
}

TEST_CASE("a queued menu-loop stop is drained before the loop is counted") {
    using namespace oscilline;
    SfxLibrary library;
    library.banks[kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[kSfxBankGame].vab, 3);
    DecodedVag& bed = library.banks[kSfxBankGame].vab.vags[0];
    bed.pcm.assign(200, 1200);
    bed.loop.has_end = true;
    bed.loop.repeat = true;
    bed.loop.start_sample = 0;
    bed.loop.end_sample = static_cast<int>(bed.pcm.size());

    auto mixer = make_mixer(&library, false);
    const auto loop_tag = static_cast<std::uint32_t>(SfxId::MenuLoop) + 1u;
    std::vector<std::int16_t> heard(32 * 2, 0);
    mixer->post(SfxId::MenuLoop);
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    mixer->stop(SfxId::MenuLoop);
    CHECK(mixer->active_with_tag(loop_tag) == 1);
    CHECK(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    SfxNote stacked;
    stacked.pcm = bed.pcm.data();
    stacked.sample_count = static_cast<int>(bed.pcm.size());
    stacked.loop = true;
    stacked.loop_start = 0;
    stacked.loop_end = stacked.sample_count;
    stacked.tag = loop_tag;
    mixer->post_note(stacked);
    mixer->post_note(stacked);
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 3);
    CHECK(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);

    mixer->set_library(&library);
    CHECK(mixer->active_with_tag(loop_tag) == 0);
    CHECK(ensure_menu_loop(*mixer));
    mixer->mix(heard, 32);
    CHECK(mixer->active_with_tag(loop_tag) == 1);
}

TEST_CASE("unidentified samples are bank bodies the event map does not play") {
    oscilline::SfxLibrary empty;
    CHECK(oscilline::unidentified_sfx(empty).empty());

    oscilline::SfxLibrary library;
    library.banks[oscilline::kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankGame].vab, 16);
    oscilline::VabBank& game = library.banks[oscilline::kSfxBankGame].vab;
    game.vags.resize(84);
    game.vags[19].pcm = {1900};
    game.vags[20].pcm = {2000};
    game.vags[40].pcm = {4000};
    game.vags[83].pcm = {8300};

    library.banks[oscilline::kSfxBankTitle].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankTitle].vab, 1);
    library.banks[oscilline::kSfxBankTutorial].loaded = true;
    fill_tone_bank(library.banks[oscilline::kSfxBankTutorial].vab, 1);

    const std::vector<oscilline::SfxBankSample> extra = oscilline::unidentified_sfx(library);
    const auto has = [&](std::uint8_t bank, int vag) {
        for (const oscilline::SfxBankSample& sample : extra) {
            if (sample.bank == bank && sample.vag == static_cast<std::uint16_t>(vag)) {
                return true;
            }
        }
        return false;
    };
    // Every live game tone 0..15 is already an SfxId, and so are VAG 19, 20, and 83.
    CHECK(extra.size() == 30);
    CHECK_FALSE(has(oscilline::kSfxBankGame, 0));
    CHECK_FALSE(has(oscilline::kSfxBankGame, 15));
    CHECK_FALSE(has(oscilline::kSfxBankGame, 19));
    CHECK_FALSE(has(oscilline::kSfxBankGame, 20));
    CHECK_FALSE(has(oscilline::kSfxBankGame, 83));
    REQUIRE(has(oscilline::kSfxBankGame, 40));
    CHECK_FALSE(has(oscilline::kSfxBankTitle, 0));
    CHECK_FALSE(has(oscilline::kSfxBankTitle, 1));
    CHECK_FALSE(has(oscilline::kSfxBankTitle, 6));
    REQUIRE(has(oscilline::kSfxBankTitle, 2));
    REQUIRE(has(oscilline::kSfxBankTutorial, 0));
    REQUIRE(has(oscilline::kSfxBankTutorial, 15));

    const oscilline::SfxBankSample& first = extra.front();
    CHECK(first.bank == oscilline::kSfxBankGame);
    CHECK(first.vag == 40);
    CHECK_FALSE(first.has_tone);

    bool saw_title_two = false;
    for (const oscilline::SfxBankSample& sample : extra) {
        if (sample.bank != oscilline::kSfxBankTitle || sample.vag != 2) {
            continue;
        }
        saw_title_two = true;
        CHECK(sample.has_tone);
        CHECK(sample.program == 0);
        CHECK(sample.tone == 2);
    }
    CHECK(saw_title_two);
    CHECK(std::size(oscilline::kSfxMap) == static_cast<std::size_t>(oscilline::SfxId::Count));
    CHECK(oscilline::kSfxMap[static_cast<std::size_t>(oscilline::SfxId::MenuMove)].tone == 6);

    oscilline::SfxNote note;
    REQUIRE(oscilline::sfx_bank_note(library, oscilline::kSfxBankGame, 40, note));
    CHECK(note.pcm == game.vags[40].pcm.data());
    CHECK(note.sample_count == 1);
    CHECK(note.rate_hz == oscilline::kSfxDefaultRateHz);
    CHECK_FALSE(note.use_adsr);
    CHECK_FALSE(oscilline::sfx_bank_note(library, oscilline::kSfxBankGame, 41, note));

    note = {};
    REQUIRE(oscilline::sfx_bank_note(library, oscilline::kSfxBankGame, 40, note));
    note.tag = 0x10000u;
    auto mixer = make_mixer(&library, true);
    CHECK_FALSE(mixer->resolve(oscilline::SfxId::MenuMove).placeholder);
    mixer->post_note(note);
    std::vector<std::int16_t> heard(8 * 2, 0);
    mixer->mix(heard, 2);
    CHECK(mixer->active_voices() == 1);
    CHECK(mixer->active_with_tag(0x10000u) == 1);
    mixer->stop_tag(0x10000u);
    mixer->mix(heard, 1);
    CHECK(mixer->active_voices() == 0);
}

TEST_CASE("music-start is title vag 17 and follows the round-start voice") {
    using namespace oscilline;
    CHECK(static_cast<int>(SfxId::MusicStart) == 47);
    CHECK(sfx_name(SfxId::MusicStart) == "music-start");
    const auto& entry = kSfxMap[static_cast<std::size_t>(SfxId::MusicStart)];
    CHECK(entry.id == SfxId::MusicStart);
    CHECK(entry.bank == kSfxBankTitle);
    CHECK(entry.program == 9);
    CHECK(entry.tone == 7);
    CHECK(entry.vag == 17);
    CHECK(entry.rate_hz == kSfxDefaultRateHz);
    CHECK_FALSE(entry.rate_confirmed);
    CHECK_FALSE(entry.loop);
    CHECK(entry.confidence == SfxConfidence::High);

    // The sting is the second part of the course-start chain, with a 500 ms
    // wait after the round-start voice. Music still waits the full prelude.
    const std::span<const SfxId> chain = course_start_sequence();
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == SfxId::RoundStart);
    CHECK(chain[1] == SfxId::MusicStart);
    CHECK(kCourseStartDelayMs == 8000);
    CHECK(kMusicStartSfxDelayMs == 1000);

    auto silent = make_mixer(nullptr, false);
    CHECK(silent->resolve(SfxId::MusicStart).placeholder);
    silent->post(SfxId::MusicStart);
    std::vector<std::int16_t> placeholder(64 * 2, 0);
    silent->mix(placeholder, 64);
    CHECK(silent->active_with_tag(static_cast<std::uint32_t>(SfxId::MusicStart) + 1u) == 1);

    SfxLibrary library;
    library.banks[kSfxBankTitle].loaded = true;
    fill_tone_bank(library.banks[kSfxBankTitle].vab, 10);
    VabBank& title = library.banks[kSfxBankTitle].vab;
    title.vags.resize(18);
    title.vags[17].pcm.assign(64, 9000);
    VabTone& tone = title.header.tones[9u * 16u + 7u];
    tone.vag = 17;
    tone.volume = 127;
    tone.pan = 64;
    tone.priority = 64;
    tone.adsr1 = 0;
    tone.adsr2 = 0;

    const std::vector<SfxBankSample> extra = unidentified_sfx(library);
    bool listed = false;
    for (const SfxBankSample& sample : extra) {
        if (sample.bank == kSfxBankTitle && sample.vag == 17) {
            listed = true;
        }
    }
    CHECK_FALSE(listed);

    auto mixer = make_mixer(&library, false);
    const SfxChoice choice = mixer->resolve(SfxId::MusicStart);
    CHECK_FALSE(choice.placeholder);
    CHECK(choice.bank == kSfxBankTitle);
    CHECK(choice.vag == 17);
    CHECK(choice.program == 9);
    CHECK(choice.tone == 7);
    CHECK(choice.rate_hz == kSfxDefaultRateHz);
    CHECK(choice.confidence == SfxConfidence::High);

    mixer->post(SfxId::MusicStart);
    std::vector<std::int16_t> heard(64 * 2, 0);
    mixer->mix(heard, 64);
    CHECK(mixer->active_with_tag(static_cast<std::uint32_t>(SfxId::MusicStart) + 1u) == 1);
    CHECK(heard != placeholder);
    bool audible = false;
    for (const std::int16_t sample : heard) {
        if (sample != 0) {
            audible = true;
            break;
        }
    }
    CHECK(audible);
}

// Output frames until a one-shot of `samples` at `rate_hz` passes its last sample.
int frames_until_sample_end(int samples, int rate_hz) {
    const auto step = static_cast<std::uint32_t>((static_cast<std::uint64_t>(rate_hz) << 16) /
                                                 oscilline::kCddaRate);
    const auto end = static_cast<std::uint32_t>(samples) << 16;
    std::uint32_t pos = 0;
    int frames = 0;
    while (pos < end && frames < oscilline::kCddaRate) {
        pos += step == 0 ? 1u : step;
        ++frames;
    }
    return frames;
}

struct StartChain {
    int cue_frames = 0;
    int gap_frames = 0;
    bool overlapped = false;
    bool sequence_waited = false;
    bool sting = false;
};

// Mixes one output frame at a time. A pause is a stretch of frames that are not mixed.
StartChain play_until_sting(oscilline::SfxMixer& mixer, int pause_after, int pause_frames) {
    StartChain result;
    std::vector<std::int16_t> buffer(2, 0);
    const int limit = oscilline::kCddaRate * 2;
    for (int frame = 0; frame < limit && !result.sting; ++frame) {
        if (frame == pause_after) {
            const bool cue = mixer.voice_tag_active(voice_tag(oscilline::SfxId::RoundStart));
            const bool sting = mixer.voice_tag_active(voice_tag(oscilline::SfxId::MusicStart));
            CHECK(cue);
            CHECK_FALSE(sting);
            frame += pause_frames;
        }
        mixer.mix(buffer, 1);
        const bool cue = mixer.voice_tag_active(voice_tag(oscilline::SfxId::RoundStart));
        const bool sting = mixer.voice_tag_active(voice_tag(oscilline::SfxId::MusicStart));
        result.sequence_waited =
            result.sequence_waited || (!cue && !sting && mixer.sequence_active());
        if (cue && sting) {
            result.overlapped = true;
        }
        if (cue) {
            ++result.cue_frames;
        } else if (!sting && result.cue_frames > 0) {
            ++result.gap_frames;
        } else if (sting) {
            result.sting = true;
        }
    }
    return result;
}

TEST_CASE("the start sting waits 1000 ms after round-start, before music") {
    using namespace oscilline;
    CHECK(kCourseStartDelayMs == 8000);
    CHECK(kMusicStartSfxDelayMs == 1000);

    auto placeholder = make_mixer(nullptr, false);
    placeholder->post_sequence(course_start_sequence(), kMusicStartSfxDelayMs);
    // A pause mid-cue must not skip ahead to the sting or to the music clock.
    const StartChain held = play_until_sting(*placeholder, 30, 50);
    CHECK_FALSE(held.overlapped);
    CHECK(held.sequence_waited);
    CHECK(held.gap_frames == kMusicStartSfxDelayMs * kCddaRate / 1000);
    CHECK(held.sting);
    CHECK(held.cue_frames > 30);
    CHECK(held.cue_frames * 1000 / kCddaRate < kCourseStartDelayMs);
    CHECK((held.cue_frames + held.gap_frames) * 1000 / kCddaRate < kCourseStartDelayMs);

    // A retry posts the chain again. The sting does not stay up from the previous run.
    placeholder->post_sequence(course_start_sequence(), kMusicStartSfxDelayMs);
    std::vector<std::int16_t> retry(2, 0);
    placeholder->mix(retry, 0);
    CHECK(placeholder->voice_tag_active(voice_tag(SfxId::RoundStart)));
    CHECK_FALSE(placeholder->voice_tag_active(voice_tag(SfxId::MusicStart)));

    constexpr int kSamples = 32;
    SfxLibrary library;
    library.banks[kSfxBankGame].loaded = true;
    fill_tone_bank(library.banks[kSfxBankGame].vab, 1);
    library.banks[kSfxBankGame].vab.vags[11].pcm.assign(kSamples, 4000);
    library.banks[kSfxBankTitle].loaded = true;
    fill_tone_bank(library.banks[kSfxBankTitle].vab, 10);
    VabBank& title = library.banks[kSfxBankTitle].vab;
    title.vags.resize(18);
    title.vags[17].pcm.assign(16, 9000);
    VabTone& tone = title.header.tones[9u * 16u + 7u];
    tone.vag = 17;
    tone.volume = 127;
    tone.pan = 64;

    auto mixer = make_mixer(&library, false);
    CHECK_FALSE(mixer->resolve(SfxId::RoundStart).placeholder);
    CHECK_FALSE(mixer->resolve(SfxId::MusicStart).placeholder);
    mixer->post_sequence(course_start_sequence(), kMusicStartSfxDelayMs);
    const StartChain played = play_until_sting(*mixer, -1, 0);
    CHECK_FALSE(played.overlapped);
    CHECK(played.sequence_waited);
    CHECK(played.gap_frames == kMusicStartSfxDelayMs * kCddaRate / 1000);
    CHECK(played.sting);
    const int sample_end = frames_until_sample_end(kSamples, 16000);
    // The voice ends when those samples have played at 16000 Hz. A short ADSR
    // release may add a few output frames. It does not wait out the prelude.
    CHECK(played.cue_frames + 1 >= sample_end);
    CHECK(played.cue_frames < sample_end + 32);
    CHECK(played.cue_frames * 1000 / kCddaRate < kCourseStartDelayMs);
    CHECK((played.cue_frames + played.gap_frames) * 1000 / kCddaRate < kCourseStartDelayMs);
}
