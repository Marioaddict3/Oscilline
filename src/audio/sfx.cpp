// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Event map, VAG lookup, and the voice mixer beside the music.

#include "oscilline/audio/sfx.hpp"

#include "oscilline/audio/cdda.hpp"
#include "oscilline/disc/iso9660.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <mutex>
#include <utility>

namespace oscilline {
namespace {

constexpr int kEnvMax = 32767;
constexpr int kDuckGain = 22937;
constexpr int kDuckStep = 512;
constexpr int kSfxEndFadeFrames = kCddaRate / 200;
constexpr char kUnmappedEvidence[] =
    "unmapped; the reference recordings do not identify this exact event yet.";
constexpr char kSuperPromoteEvidence[] =
    "Footage of the original: the 18th clear promotes rabbit to super and plays "
    "PSJ_SE VAG index 19 (program 1 tone 14) at 11025 Hz, one-shot, with no music duck. High.";
constexpr char kClearBlockEvidence[] = "PAL DuckStation success capture: GAME sample 004 "
                                       "correlates with repeated block-success moments.";
constexpr char kClearPitEvidence[] = "PAL DuckStation success capture: GAME sample 005 correlates "
                                     "with repeated pit-success moments.";
constexpr char kClearLoopEvidence[] =
    "PAL longplay repeated loop success frames and PCM comparison confirm GAME VAG 6 at "
    "11025 Hz; JP longplay corroborates (docs/sfx-comparison.md).";
constexpr char kClearWaveEvidence[] =
    "PAL longplay repeated wave success frames and PCM comparison confirm GAME VAG 7 at "
    "11025 Hz; JP longplay corroborates (docs/sfx-comparison.md).";
constexpr char kClearBlockLoopEvidence[] =
    "PAL longplay repeated block+loop success frames and PCM comparison confirm GAME VAG 9 at "
    "11025 Hz; JP longplay corroborates (docs/sfx-comparison.md).";
constexpr char kClearBlockPitEvidence[] =
    "PAL longplay repeated block+pit frames and distinctive PCM prefixes match GAME VAG 8 at "
    "11025 Hz (docs/sfx-comparison.md).";
constexpr char kClearBlockWaveEvidence[] =
    "PAL longplay repeated block+wave frames and distinctive PCM prefixes match GAME VAG 10 at "
    "11025 Hz; PAL Auto-mode and JP corroborate (docs/sfx-comparison.md).";
constexpr char kClearPitLoopEvidence[] =
    "PAL longplay repeated pit+loop frames and distinctive PCM prefixes match GAME VAG 11 at "
    "11025 Hz; JP corroborates (docs/sfx-comparison.md).";
constexpr char kClearPitWaveEvidence[] =
    "PAL repeated pit+wave successes and CD-music-adjusted PCM comparisons confirm GAME VAG 12 "
    "at 11025 Hz; JP corroborates (docs/sfx-comparison.md).";
constexpr char kClearLoopWaveEvidence[] =
    "PAL longplay repeated loop+wave frames and distinctive PCM prefixes match GAME VAG 13 at "
    "11025 Hz; JP corroborates (docs/sfx-comparison.md).";
constexpr char kMissEvidence[] =
    "Black-box SPU key-on: a wrong press or no press into an obstacle is GAME program 1 "
    "tone 10 at 11025 Hz.";
constexpr char kMenuSelectEvidence[] =
    "Black-box SPU key-on: menu select is TITLE program 0 tone 1 at 9991 Hz.";
constexpr char kMenuSelectFollowEvidence[] =
    "Black-box SPU key-on: menu-select follow is the PSJ_SE menu-loop sample, program 2 "
    "tone 0, at 11025 Hz. PSJ_SE is the GAME/AUDIO bank.";
constexpr char kMenuBackEvidence[] =
    "Black-box SPU key-on: menu back is the PSJ_SE \"Back\" line at VAG index 20 "
    "(program 6 tone 0), 9991 Hz (SPU pitch 0x03A0). It plays on triangle in the wheel "
    "contexts. Triangle on the title list or the language grid is silent. PSJ_SE is the "
    "GAME/AUDIO bank.";
constexpr char kMenuMoveEvidence[] =
    "Black-box SPU key-on: cursor move on the title menu and the language grid is TITLE "
    "program 0 tone 6 at 9991 Hz (SPU pitch 0x03A0), one voice. An edge move, or "
    "left/right on a vertical list, is silent.";
constexpr char kTitleConfirmEvidence[] =
    "Black-box SPU key-on: title confirm is TITLE program 0 tone 0 at 9991 Hz.";
constexpr char kWheelAnnounceEvidence[] =
    "Black-box SPU key-on: a PSJ_SE wheel announcement at 9991 Hz. Parts of one line "
    "start when the previous part ends and are never keyed together.";
constexpr char kWhiffEvidence[] =
    "Black-box SPU key-on: a press with no obstacle is GAME program 1 tone 11 at 11025 Hz.";
constexpr char kWhiffAltEvidence[] =
    "Black-box SPU key-on: an alternate whiff is GAME program 1 tone 15 at 12371 Hz. "
    "Medium, and not layered on the empty press unless both whiffs are required.";
constexpr char kPostCrashEvidence[] =
    "Black-box SPU key-on: the first press after a crash is often GAME program 1 tone 12 "
    "at 12371 Hz. Medium; gameplay posts it only with --sfx-experimental.";
constexpr char kDifficultyConfirmEvidence[] =
    "Black-box SPU key-on: wheel CROSS starts with program 0 tone 1 at 9991 Hz (SPU pitch "
    "0x03A0). TITLE VAG 19 and PSJ_SE VAG 16 are the same sample.";
constexpr char kDifficultyConfirmFollowEvidence[] =
    "Black-box SPU key-on: difficulty-confirm-b is the PSJ_SE sample at VAG index 83. "
    "Medium; the disc sample stays off unless --sfx-experimental is set.";
constexpr char kMenuLoopEvidence[] = "PAL DuckStation capture: user identified GAME VAG 14 as the "
                                     "main-menu loop; 11025 Hz is closest.";
constexpr char kRoundStartEvidence[] =
    "PAL DuckStation course-start recording: GAME VAG 83 correlates at 16000 Hz; "
    "JP longplay corroborates the rate (docs/sfx-comparison.md).";
constexpr char kGameOverEvidence[] =
    "PAL DuckStation capture: user identified GAME VAG 2 as game-over music. "
    "16000 Hz is an estimate, the same in-between rate as round start and level "
    "complete. A verified event and rate match was not found.";
constexpr char kLevelCompleteEvidence[] =
    "PAL longplay three Clear transitions match GAME VAG 3 at 16000 Hz, including whole-cue "
    "comparison; JP longplay corroborates (docs/sfx-comparison.md).";
constexpr char kMusicStartEvidence[] =
    "Viewer unidentified row 115: TITLE bank VAG index 17, program 9 tone 7. "
    "Chained 500 ms after round start ends, during the prelude, before music. "
    "11025 Hz is the unidentified-row audition rate. One-shot.";

constexpr const char* kSfxNames[] = {
    "menu-move",
    "menu-select",
    "menu-back",
    "menu-loop",
    "cleared-block",
    "cleared-pit",
    "cleared-loop",
    "cleared-wave",
    "cleared-block+pit",
    "cleared-block+loop",
    "cleared-block+wave",
    "cleared-pit+loop",
    "cleared-pit+wave",
    "cleared-loop+wave",
    "missed-block",
    "missed-pit",
    "missed-loop",
    "missed-wave",
    "missed-block+pit",
    "missed-block+loop",
    "missed-block+wave",
    "missed-pit+loop",
    "missed-pit+wave",
    "missed-loop+wave",
    "form-change",
    "round-start",
    "game-over",
    "level-complete",
    "whiff",
    "whiff-alt",
    "post-crash-press",
    "menu-select-follow",
    "difficulty-confirm",
    "difficulty-confirm-b",
    "title-confirm",
    "announce-disc-0",
    "announce-disc-1",
    "announce-join",
    "announce-cd-0",
    "announce-cd-1",
    "announce-options",
    "announce-back",
    "announce-bronze",
    "announce-silver",
    "announce-gold",
    "announce-scores",
    "super-promote",
    "music-start",
};

static_assert(std::size(kSfxNames) == static_cast<std::size_t>(SfxId::Count));

constexpr const char* kBankLabel[kSfxBankCount] = {"game", "title", "tutorial"};
constexpr const char* kBankPrefix[kSfxBankCount] = {"game/audio/", "title/audio/", "kiosk/audio/"};
constexpr const char* kBankStem[kSfxBankCount] = {"psj_se", "title", "kiosk"};

std::string canonical_path(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch == '\\') {
            ch = '/';
        }
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

int sat16(std::int32_t sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return static_cast<int>(sample);
}

int shift_step(int step, int shift) {
    const int places = std::max(0, 11 - shift);
    const int magnitude = step < 0 ? -step : step;
    const int scaled = magnitude << places;
    return step < 0 ? -scaled : scaled;
}

// SPU ADSR step from the public notes: shift, the 0x6000 exponential-attack
// slowdown, and a decay amount scaled by the current level.
void apply_rate(AdsrState& state) {
    int cycles = 1 << std::max(0, state.shift - 11);
    int amount = shift_step(state.base_step, state.shift);
    if (state.exponential && state.increase && state.level > 0x6000) {
        cycles <<= 2;
    }
    if (state.exponential && !state.increase) {
        amount = static_cast<int>((static_cast<std::int64_t>(amount) * state.level) / 32768);
        if (amount == 0 && state.level > 0 && state.base_step < 0) {
            amount = -1;
        }
    }
    if (cycles < 1) {
        cycles = 1;
    }
    state.cycles = cycles;
    state.amount = amount;
}

void enter_phase(
    AdsrState& state, int phase, int shift, int base_step, bool exponential, bool increase) {
    state.phase = phase;
    state.shift = shift;
    state.base_step = base_step;
    state.exponential = exponential;
    state.increase = increase;
    state.wait = 0;
    apply_rate(state);
}

int attack_step(int index) {
    constexpr int kSteps[4] = {7, 6, 5, 4};
    return kSteps[index & 3];
}

int sustain_step(int index, bool decrease) {
    if (!decrease) {
        return attack_step(index);
    }
    constexpr int kSteps[4] = {-8, -7, -6, -5};
    return kSteps[index & 3];
}

int sustain_of(std::uint16_t adsr1) {
    const int nibble = adsr1 & 0x0F;
    const int level = (nibble + 1) * 0x800;
    return std::min(level, kEnvMax);
}

void program_attack(AdsrState& state) {
    const int mode = (state.adsr1 >> 15) & 1;
    const int shift = (state.adsr1 >> 10) & 31;
    const int step = attack_step((state.adsr1 >> 8) & 3);
    enter_phase(state, 0, shift, step, mode != 0, true);
}

void program_decay(AdsrState& state) {
    const int shift = (state.adsr1 >> 4) & 15;
    enter_phase(state, 1, shift, -8, true, false);
}

void program_sustain(AdsrState& state) {
    const int mode = (state.adsr2 >> 15) & 1;
    const bool decrease = ((state.adsr2 >> 14) & 1) != 0;
    const int shift = (state.adsr2 >> 8) & 31;
    const int step = sustain_step((state.adsr2 >> 6) & 3, decrease);
    enter_phase(state, 2, shift, step, mode != 0, !decrease);
}

void program_release(AdsrState& state) {
    const int mode = (state.adsr2 >> 5) & 1;
    const int shift = state.adsr2 & 31;
    enter_phase(state, 3, shift, -8, mode != 0, false);
}

std::uint32_t step_for_rate(int rate_hz) {
    if (rate_hz < 1) {
        rate_hz = 1;
    }
    const auto step =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(rate_hz) << 16) / kCddaRate);
    return step == 0 ? 1u : step;
}

void step_gain(int& gain, int target) {
    if (gain < target) {
        gain = std::min(target, gain + kDuckStep);
    } else if (gain > target) {
        gain = std::max(target, gain - kDuckStep);
    }
}

// Original waveforms. These are not taken from a disc.
void write_tone(
    std::vector<std::int16_t>& out, int freq_hz, int frames, int amp, std::uint32_t& phase) {
    const auto step =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(freq_hz) << 32) / kCddaRate);
    for (int i = 0; i < frames; ++i) {
        const std::uint32_t rising = phase < 0x80000000u ? phase : (0xFFFFFFFFu - phase);
        const int tri = static_cast<int>(rising >> 16) - 16384;
        const int env = frames <= 1 ? amp : amp * (frames - i) / frames;
        const int sample =
            sat16(static_cast<std::int32_t>((static_cast<std::int64_t>(tri) * env) >> 14));
        out.push_back(static_cast<std::int16_t>(sample));
        phase += step;
    }
}

int ms_to_frames(int ms) {
    return std::max(1, kCddaRate * ms / 1000);
}

std::vector<std::int16_t> make_placeholder(SfxId id) {
    std::vector<std::int16_t> pcm;
    std::uint32_t phase = 0;
    const int index = static_cast<int>(id);
    const int cleared = static_cast<int>(SfxId::Cleared);
    const int missed = static_cast<int>(SfxId::Missed);
    const int form = static_cast<int>(SfxId::FormChange);
    if (id == SfxId::MenuMove) {
        write_tone(pcm, 980, ms_to_frames(45), 9000, phase);
    } else if (id == SfxId::MenuSelect) {
        write_tone(pcm, 740, ms_to_frames(40), 10000, phase);
        write_tone(pcm, 1180, ms_to_frames(50), 10000, phase);
    } else if (id == SfxId::MenuBack) {
        write_tone(pcm, 420, ms_to_frames(70), 9000, phase);
    } else if (index >= cleared && index < missed) {
        write_tone(pcm, 1320 + (index - cleared) * 40, ms_to_frames(80), 11000, phase);
    } else if (index >= missed && index < form) {
        write_tone(pcm, 140 + (index - missed) * 12, ms_to_frames(110), 8000, phase);
    } else if (id == SfxId::FormChange) {
        const int frames = ms_to_frames(160);
        for (int i = 0; i < frames; ++i) {
            const int freq = 360 + (520 * i) / frames;
            const auto step =
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(freq) << 32) / kCddaRate);
            const std::uint32_t rising = phase < 0x80000000u ? phase : (0xFFFFFFFFu - phase);
            const int tri = static_cast<int>(rising >> 16) - 16384;
            const int env = 10000 * (frames - i) / frames;
            pcm.push_back(static_cast<std::int16_t>(
                sat16(static_cast<std::int32_t>((static_cast<std::int64_t>(tri) * env) >> 14))));
            phase += step;
        }
    } else if (id == SfxId::RoundStart) {
        write_tone(pcm, 523, ms_to_frames(60), 10000, phase);
        write_tone(pcm, 659, ms_to_frames(60), 10000, phase);
        write_tone(pcm, 784, ms_to_frames(80), 10000, phase);
    } else if (id == SfxId::Whiff || id == SfxId::WhiffAlt) {
        write_tone(pcm, 210, ms_to_frames(50), 7000, phase);
    } else if (id == SfxId::PostCrashPress) {
        write_tone(pcm, 160, ms_to_frames(70), 7000, phase);
    } else if (id == SfxId::MenuSelectFollow) {
        write_tone(pcm, 1480, ms_to_frames(40), 9000, phase);
    } else if (id == SfxId::DifficultyConfirm || id == SfxId::DifficultyConfirmFollow) {
        write_tone(pcm, 880, ms_to_frames(45), 10000, phase);
        write_tone(pcm, 1320, ms_to_frames(40), 10000, phase);
    } else {
        write_tone(pcm, 784, ms_to_frames(70), 9000, phase);
        write_tone(pcm, 392, ms_to_frames(100), 9000, phase);
    }
    return pcm;
}

} // namespace

int SfxMixer::sample_at(const Voice& voice, int index) {
    if (voice.pcm == nullptr || voice.sample_count <= 0) {
        return 0;
    }
    if (voice.loop) {
        const int span = voice.loop_end - voice.loop_start;
        if (span > 0 && index >= voice.loop_start) {
            int rel = (index - voice.loop_start) % span;
            if (rel < 0) {
                rel += span;
            }
            index = voice.loop_start + rel;
        }
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= voice.sample_count) {
        index = voice.sample_count - 1;
    }
    return voice.pcm[index];
}

int lobe_weight(int dist_q16) {
    // A four-tap lobe of our own. It is not the SPU gaussian table.
    // Weight falls to zero a little past two samples.
    const auto d2 = (static_cast<std::int64_t>(dist_q16) * dist_q16) >> 16;
    const auto cut = (d2 * 3) >> 2;
    if (cut >= 65536) {
        return 0;
    }
    return static_cast<int>(65536 - cut);
}

int SfxMixer::interpolate(const Voice& voice) {
    const int index = static_cast<int>(voice.pos_q16 >> 16);
    const int frac = static_cast<int>(voice.pos_q16 & 0xFFFFu);
    if (voice.resample == SfxResample::Linear || voice.sample_count < 2) {
        const int s0 = sample_at(voice, index);
        const int s1 = sample_at(voice, index + 1);
        return s0 + static_cast<int>((static_cast<std::int64_t>(s1 - s0) * frac) >> 16);
    }
    const int dist[4] = {65536 + frac, frac, 65536 - frac, 131072 - frac};
    int weight[4];
    int sum = 0;
    for (int tap = 0; tap < 4; ++tap) {
        weight[tap] = lobe_weight(dist[tap]);
        sum += weight[tap];
    }
    if (sum <= 0) {
        return sample_at(voice, index);
    }
    std::int64_t acc = 0;
    for (int tap = 0; tap < 4; ++tap) {
        acc += static_cast<std::int64_t>(sample_at(voice, index - 1 + tap)) * weight[tap];
    }
    return static_cast<int>(acc / sum);
}

static int channel_gain(
    int bank_master, int program_volume, int tone_volume, int pan, bool left, int master_q15) {
    int span = left ? (127 - pan) : pan;
    int shaped = span * 2;
    if (shaped > 127) {
        shaped = 127;
    }
    const std::int64_t num =
        static_cast<std::int64_t>(bank_master) * program_volume * tone_volume * shaped * master_q15;
    const std::int64_t den = 127ll * 127 * 127 * 127;
    int gain = static_cast<int>(num / den);
    if (gain > kEnvMax) {
        gain = kEnvMax;
    }
    if (gain < 0) {
        gain = 0;
    }
    return gain;
}

void SfxMixer::advance_voice(Voice& voice) {
    voice.pos_q16 += voice.step_q16;
    if (voice.loop) {
        const auto end_q = static_cast<std::uint32_t>(voice.loop_end) << 16;
        const auto start_q = static_cast<std::uint32_t>(voice.loop_start) << 16;
        if (end_q > start_q && voice.pos_q16 >= end_q) {
            const std::uint32_t span = end_q - start_q;
            voice.pos_q16 = start_q + (voice.pos_q16 - end_q) % span;
        }
        return;
    }
    const auto end_q = static_cast<std::uint32_t>(std::max(voice.sample_count, 0)) << 16;
    if (voice.pos_q16 < end_q) {
        return;
    }
    if (voice.use_adsr && voice.release_at_end && voice.adsr.phase < 3) {
        adsr_key_off(voice.adsr);
        voice.release_at_end = false;
        if (voice.sample_count > 0) {
            voice.pos_q16 = (static_cast<std::uint32_t>(voice.sample_count - 1) << 16);
        }
        return;
    }
    voice.active = false;
}

bool SfxMixer::less_valuable(const Voice& left, const Voice& right) {
    const auto value = [](const Voice& voice) {
        const int level = voice.use_adsr ? voice.adsr.level : kEnvMax;
        const auto gain =
            static_cast<std::int64_t>(std::abs(voice.gain_l_q15) + std::abs(voice.gain_r_q15));
        const std::int64_t loud = static_cast<std::int64_t>(std::min(level, kEnvMax)) * gain;
        return (static_cast<std::int64_t>(voice.priority) << 48) | (loud & 0xFFFFFFFFFFFFLL);
    };
    const auto a = value(left);
    const auto b = value(right);
    if (a != b) {
        return a < b;
    }
    return left.age < right.age;
}

void adsr_key_on(AdsrState& state, std::uint16_t adsr1, std::uint16_t adsr2) {
    state.adsr1 = adsr1;
    state.adsr2 = adsr2;
    state.level = 0;
    state.sustain_level = sustain_of(adsr1);
    program_attack(state);
}

void adsr_key_off(AdsrState& state) {
    if (state.phase == 4) {
        return;
    }
    program_release(state);
}

int adsr_tick(AdsrState& state) {
    if (state.phase == 4) {
        state.level = 0;
        return 0;
    }
    apply_rate(state);
    ++state.wait;
    if (state.wait >= state.cycles) {
        state.wait = 0;
        int level = state.level + state.amount;
        if (level > kEnvMax) {
            level = kEnvMax;
        }
        if (level < 0) {
            level = 0;
        }
        state.level = level;
    }
    if (state.phase == 0 && state.level >= kEnvMax) {
        state.level = kEnvMax;
        program_decay(state);
    } else if (state.phase == 1 && state.level <= state.sustain_level) {
        state.level = state.sustain_level;
        program_sustain(state);
    } else if (state.phase == 3 && state.level <= 0) {
        state.level = 0;
        state.phase = 4;
    }
    return state.level;
}

const SfxMapEntry kSfxMap[static_cast<std::size_t>(SfxId::Count)] = {
    {SfxId::MenuMove,
     kSfxBankTitle,
     0,
     6,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kMenuMoveEvidence},
    {SfxId::MenuSelect,
     kSfxBankTitle,
     0,
     1,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kMenuSelectEvidence},
    {SfxId::MenuBack,
     kSfxBankGame,
     0,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kMenuBackEvidence,
     false,
     20},
    {SfxId::MenuLoop,
     kSfxBankGame,
     2,
     0,
     11025,
     true,
     SfxConfidence::High,
     kMenuLoopEvidence,
     true},
    {sfx_cleared(0), kSfxBankGame, 1, 0, 11025, true, SfxConfidence::High, kClearBlockEvidence},
    {sfx_cleared(1), kSfxBankGame, 1, 1, 11025, true, SfxConfidence::High, kClearPitEvidence},
    {sfx_cleared(2), kSfxBankGame, 1, 2, 11025, true, SfxConfidence::High, kClearLoopEvidence},
    {sfx_cleared(3), kSfxBankGame, 1, 3, 11025, true, SfxConfidence::High, kClearWaveEvidence},
    {sfx_cleared(4), kSfxBankGame, 1, 4, 11025, true, SfxConfidence::High, kClearBlockPitEvidence},
    {sfx_cleared(5), kSfxBankGame, 1, 5, 11025, true, SfxConfidence::High, kClearBlockLoopEvidence},
    {sfx_cleared(6), kSfxBankGame, 1, 6, 11025, true, SfxConfidence::High, kClearBlockWaveEvidence},
    {sfx_cleared(7), kSfxBankGame, 1, 7, 11025, true, SfxConfidence::High, kClearPitLoopEvidence},
    {sfx_cleared(8), kSfxBankGame, 1, 8, 11025, true, SfxConfidence::High, kClearPitWaveEvidence},
    {sfx_cleared(9), kSfxBankGame, 1, 9, 11025, true, SfxConfidence::High, kClearLoopWaveEvidence},
    {sfx_missed(0), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(1), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(2), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(3), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(4), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(5), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(6), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(7), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(8), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {sfx_missed(9), kSfxBankGame, 1, 10, 11025, true, SfxConfidence::High, kMissEvidence},
    {SfxId::FormChange,
     kSfxBankNone,
     0,
     0,
     kSfxDefaultRateHz,
     false,
     SfxConfidence::Low,
     kUnmappedEvidence},
    {SfxId::RoundStart, kSfxBankGame, 0, 11, 16000, true, SfxConfidence::High, kRoundStartEvidence},
    {SfxId::GameOver, kSfxBankGame, 0, 12, 16000, false, SfxConfidence::High, kGameOverEvidence},
    {SfxId::LevelComplete,
     kSfxBankGame,
     0,
     13,
     16000,
     true,
     SfxConfidence::High,
     kLevelCompleteEvidence},
    {SfxId::Whiff, kSfxBankGame, 1, 11, 11025, true, SfxConfidence::High, kWhiffEvidence},
    {SfxId::WhiffAlt, kSfxBankGame, 1, 15, 12371, true, SfxConfidence::Medium, kWhiffAltEvidence},
    {SfxId::PostCrashPress,
     kSfxBankGame,
     1,
     12,
     12371,
     true,
     SfxConfidence::Medium,
     kPostCrashEvidence},
    {SfxId::MenuSelectFollow,
     kSfxBankGame,
     2,
     0,
     11025,
     true,
     SfxConfidence::High,
     kMenuSelectFollowEvidence},
    {SfxId::DifficultyConfirm,
     kSfxBankTitle,
     0,
     1,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kDifficultyConfirmEvidence},
    {SfxId::DifficultyConfirmFollow,
     kSfxBankGame,
     0,
     0,
     kSfxDefaultRateHz,
     false,
     SfxConfidence::Medium,
     kDifficultyConfirmFollowEvidence,
     false,
     83},
    {SfxId::TitleConfirm,
     kSfxBankTitle,
     0,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kTitleConfirmEvidence},
    {SfxId::AnnounceDisc0,
     kSfxBankGame,
     15,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceDisc1,
     kSfxBankGame,
     15,
     1,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceJoin,
     kSfxBankGame,
     7,
     14,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceCd0,
     kSfxBankGame,
     7,
     13,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceCd1,
     kSfxBankGame,
     15,
     3,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceOptions,
     kSfxBankGame,
     7,
     7,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceBack,
     kSfxBankGame,
     6,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceBronze,
     kSfxBankGame,
     7,
     2,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceSilver,
     kSfxBankGame,
     7,
     1,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceGold,
     kSfxBankGame,
     7,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::AnnounceScores,
     kSfxBankGame,
     8,
     0,
     kSfxPitch9991Hz,
     true,
     SfxConfidence::High,
     kWheelAnnounceEvidence},
    {SfxId::SuperPromote,
     kSfxBankGame,
     1,
     14,
     11025,
     true,
     SfxConfidence::High,
     kSuperPromoteEvidence,
     false,
     19},
    {SfxId::MusicStart,
     kSfxBankTitle,
     9,
     7,
     kSfxDefaultRateHz,
     false,
     SfxConfidence::High,
     kMusicStartEvidence,
     false,
     17},
};

std::string_view sfx_name(SfxId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= std::size(kSfxNames)) {
        return "unknown";
    }
    return kSfxNames[index];
}

SfxLibrary load_sfx_library(const IsoVolume& volume) {
    struct Pair {
        std::string stem;
        const FsNode* vh = nullptr;
        const FsNode* vb = nullptr;
    };
    SfxLibrary library;
    for (std::uint8_t bank = 0; bank < kSfxBankCount; ++bank) {
        std::vector<Pair> pairs;
        const std::string prefix = kBankPrefix[bank];
        for (const FsNode& node : volume.nodes()) {
            if (node.directory || node.outside_data_track) {
                continue;
            }
            const std::string path = canonical_path(node.path);
            if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const bool vh = path.size() >= 3 && path.compare(path.size() - 3, 3, ".vh") == 0;
            const bool vb = path.size() >= 3 && path.compare(path.size() - 3, 3, ".vb") == 0;
            if (!vh && !vb) {
                continue;
            }
            const std::string stem = path.substr(0, path.size() - 3);
            Pair* pair = nullptr;
            for (Pair& existing : pairs) {
                if (existing.stem == stem) {
                    pair = &existing;
                    break;
                }
            }
            if (pair == nullptr) {
                pairs.push_back(Pair{stem, nullptr, nullptr});
                pair = &pairs.back();
            }
            if (vh) {
                pair->vh = &node;
            } else {
                pair->vb = &node;
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            return a.stem < b.stem;
        });
        const std::string preferred = prefix + kBankStem[bank];
        const Pair* chosen = nullptr;
        const Pair* fallback = nullptr;
        for (const Pair& pair : pairs) {
            if (pair.vh == nullptr || pair.vb == nullptr) {
                continue;
            }
            if (fallback == nullptr) {
                fallback = &pair;
            }
            if (pair.stem == preferred) {
                chosen = &pair;
                break;
            }
        }
        if (chosen == nullptr) {
            chosen = fallback;
        }
        if (chosen == nullptr) {
            continue;
        }
        if (pairs.size() > 1) {
            std::cerr << "oscilline: sfx " << kBankLabel[bank]
                      << " has more than one VH/VB pair; using " << chosen->vh->path << '\n';
        }
        auto header = volume.read_file(*chosen->vh);
        if (!header) {
            std::cerr << "oscilline: sfx " << kBankLabel[bank] << ": " << header.error() << '\n';
            continue;
        }
        auto body = volume.read_file(*chosen->vb);
        if (!body) {
            std::cerr << "oscilline: sfx " << kBankLabel[bank] << ": " << body.error() << '\n';
            continue;
        }
        auto parsed = parse_vab(header.value(), body.value());
        if (!parsed) {
            std::cerr << "oscilline: sfx " << kBankLabel[bank] << ": " << parsed.error() << '\n';
            continue;
        }
        library.banks[bank].loaded = true;
        library.banks[bank].vab = std::move(parsed.value());
        library.banks[bank].path = chosen->vh->path;
        std::cerr << "oscilline: sfx " << kBankLabel[bank] << " from " << chosen->vh->path << '\n';
    }
    return library;
}

std::vector<SfxBankSample> unidentified_sfx(const SfxLibrary& library) {
    std::vector<SfxBankSample> found;
    for (std::uint8_t bank = 0; bank < kSfxBankCount; ++bank) {
        if (!library.banks[bank].loaded) {
            continue;
        }
        const VabBank& vab = library.banks[bank].vab;
        std::vector<std::uint8_t> identified(vab.vags.size(), 0);
        for (const SfxMapEntry& entry : kSfxMap) {
            if (entry.bank != bank) {
                continue;
            }
            std::uint16_t slot = kSfxVagUnset;
            bool have = false;
            if (entry.vag != kSfxVagUnset) {
                slot = entry.vag;
                have = true;
            } else if (entry.program < vab.header.program_count && entry.tone < 16) {
                const std::size_t tone_index =
                    static_cast<std::size_t>(entry.program) * 16u + entry.tone;
                if (tone_index < vab.header.tones.size()) {
                    slot = vab.header.tones[tone_index].vag;
                    have = true;
                }
            }
            if (have && static_cast<std::size_t>(slot) < identified.size()) {
                identified[slot] = 1;
            }
        }

        std::vector<std::uint8_t> have_tone(vab.vags.size(), 0);
        std::vector<std::uint8_t> program_of(vab.vags.size(), 0);
        std::vector<std::uint8_t> tone_of(vab.vags.size(), 0);
        const std::size_t programs = std::min(vab.header.programs.size(),
                                              static_cast<std::size_t>(vab.header.program_count));
        for (std::size_t program = 0; program < programs; ++program) {
            int live = vab.header.programs[program].tone_count;
            if (live > 16) {
                live = 16;
            }
            for (int tone = 0; tone < live; ++tone) {
                const std::size_t tone_index = program * 16u + static_cast<std::size_t>(tone);
                if (tone_index >= vab.header.tones.size()) {
                    break;
                }
                const std::uint16_t vag = vab.header.tones[tone_index].vag;
                if (static_cast<std::size_t>(vag) >= vab.vags.size() || have_tone[vag] != 0) {
                    continue;
                }
                have_tone[vag] = 1;
                program_of[vag] = static_cast<std::uint8_t>(program);
                tone_of[vag] = static_cast<std::uint8_t>(tone);
            }
        }

        for (std::size_t vag = 0; vag < vab.vags.size(); ++vag) {
            if (vab.vags[vag].pcm.empty() || identified[vag] != 0) {
                continue;
            }
            SfxBankSample sample;
            sample.bank = bank;
            sample.vag = static_cast<std::uint16_t>(vag);
            sample.has_tone = have_tone[vag] != 0;
            if (sample.has_tone) {
                sample.program = program_of[vag];
                sample.tone = tone_of[vag];
            }
            found.push_back(sample);
        }
    }
    return found;
}

bool sfx_bank_note(const SfxLibrary& library, std::uint8_t bank, std::uint16_t vag, SfxNote& note) {
    if (bank >= kSfxBankCount || !library.banks[bank].loaded) {
        return false;
    }
    const VabBank& vab = library.banks[bank].vab;
    if (static_cast<std::size_t>(vag) >= vab.vags.size()) {
        return false;
    }
    const DecodedVag& decoded = vab.vags[vag];
    if (decoded.pcm.empty()) {
        return false;
    }
    const VabTone* tone = nullptr;
    const VabProgram* program = nullptr;
    const std::size_t programs =
        std::min(vab.header.programs.size(), static_cast<std::size_t>(vab.header.program_count));
    for (std::size_t program_index = 0; program_index < programs && tone == nullptr;
         ++program_index) {
        int live = vab.header.programs[program_index].tone_count;
        if (live > 16) {
            live = 16;
        }
        for (int tone_index = 0; tone_index < live; ++tone_index) {
            const std::size_t slot = program_index * 16u + static_cast<std::size_t>(tone_index);
            if (slot >= vab.header.tones.size()) {
                break;
            }
            if (vab.header.tones[slot].vag != vag) {
                continue;
            }
            tone = &vab.header.tones[slot];
            program = &vab.header.programs[program_index];
            break;
        }
    }
    note = SfxNote{};
    note.pcm = decoded.pcm.data();
    note.sample_count = static_cast<int>(decoded.pcm.size());
    note.rate_hz = kSfxDefaultRateHz;
    note.use_adsr = tone != nullptr;
    if (tone != nullptr) {
        note.adsr1 = tone->adsr1;
        note.adsr2 = tone->adsr2;
        note.priority = tone->priority;
    }
    const int program_volume = program != nullptr ? program->volume : 127;
    const int tone_volume = tone != nullptr ? tone->volume : 127;
    const int pan = tone != nullptr ? tone->pan : 64;
    note.gain_l_q15 =
        channel_gain(vab.header.master_volume, program_volume, tone_volume, pan, true, kEnvMax);
    note.gain_r_q15 =
        channel_gain(vab.header.master_volume, program_volume, tone_volume, pan, false, kEnvMax);
    if (decoded.loop.has_end && decoded.loop.repeat &&
        decoded.loop.end_sample > decoded.loop.start_sample &&
        decoded.loop.end_sample <= note.sample_count) {
        note.loop = true;
        note.loop_start = decoded.loop.has_start ? decoded.loop.start_sample : 0;
        note.loop_end = decoded.loop.end_sample;
        if (note.loop_start >= note.loop_end) {
            note.loop_start = 0;
        }
    }
    note.release_at_end = !(decoded.loop.has_end && !decoded.loop.repeat);
    note.fade_at_end = true;
    return true;
}

SfxMixer::SfxMixer(const SfxLibrary* library, std::span<const SfxMapEntry> map, SfxConfig config)
    : library_(library), map_(map.begin(), map.end()), config_(config), queue_(kQueue) {
    voices_.assign(static_cast<std::size_t>(kSfxVoiceLimit), Voice{});
    for (std::size_t i = 0; i < static_cast<std::size_t>(SfxId::Count); ++i) {
        placeholders_[i] = make_placeholder(static_cast<SfxId>(i));
    }
}

void SfxMixer::set_library(const SfxLibrary* library) {
    library_ = library;
    for (Voice& voice : voices_) {
        voice.active = false;
    }
}

void SfxMixer::set_experimental(bool experimental) {
    config_.experimental = experimental;
}

void SfxMixer::set_sfx_gain_q15(int gain_q15) {
    if (gain_q15 < 0) {
        gain_q15 = 0;
    }
    if (gain_q15 > kEnvMax) {
        gain_q15 = kEnvMax;
    }
    sfx_gain_.store(gain_q15, std::memory_order_relaxed);
}

void SfxMixer::enqueue(const Command& command) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    if (queue_count_ == kQueue) {
        queue_read_ = (queue_read_ + 1) % kQueue;
        --queue_count_;
    }
    const int slot = (queue_read_ + queue_count_) % kQueue;
    queue_[static_cast<std::size_t>(slot)] = command;
    ++queue_count_;
}

void SfxMixer::post(SfxId id) {
    Command command;
    command.kind = Command::Kind::Event;
    command.id = id;
    enqueue(command);
}

void SfxMixer::stop(SfxId id) {
    Command command;
    command.kind = Command::Kind::Stop;
    command.id = id;
    enqueue(command);
}

void SfxMixer::stop_tag(std::uint32_t tag) {
    Command command;
    command.kind = Command::Kind::StopTag;
    command.tag = tag;
    enqueue(command);
}

void SfxMixer::post_note(const SfxNote& note) {
    Command command;
    command.kind = Command::Kind::Note;
    command.note = note;
    enqueue(command);
}

void SfxMixer::post_sequence(std::span<const SfxId> ids, int inter_part_delay_ms) {
    if (ids.empty()) {
        return;
    }
    Command command;
    command.kind = Command::Kind::Sequence;
    const int count = std::min(static_cast<int>(ids.size()), kSfxSequenceMax);
    command.sequence_count = count;
    command.sequence_delay_ms = std::clamp(inter_part_delay_ms, 0, 30000);
    for (int i = 0; i < count; ++i) {
        command.sequence[static_cast<std::size_t>(i)] = ids[static_cast<std::size_t>(i)];
    }
    enqueue(command);
}

void SfxMixer::silence_tag(std::uint32_t tag) {
    if (tag == 0) {
        return;
    }
    for (Voice& voice : voices_) {
        if (voice.active && voice.tag == tag) {
            voice.active = false;
        }
    }
}

void SfxMixer::begin_sequence(const SfxId* ids, int count, int inter_part_delay_ms) {
    if (ids == nullptr || count <= 0) {
        return;
    }
    if (sequence_live_) {
        silence_tag(sequence_tag_);
        sequence_live_ = false;
    }
    if (count > kSfxSequenceMax) {
        count = kSfxSequenceMax;
    }
    sequence_count_ = count;
    sequence_pos_ = 0;
    const std::int64_t delay_frames =
        static_cast<std::int64_t>(kCddaRate) * std::max(inter_part_delay_ms, 0) / 1000;
    sequence_delay_frames_ = static_cast<int>(delay_frames);
    sequence_wait_frames_ = 0;
    for (int i = 0; i < count; ++i) {
        sequence_[static_cast<std::size_t>(i)] = ids[i];
    }
    start_event(sequence_[0]);
    sequence_tag_ = static_cast<std::uint32_t>(sequence_[0]) + 1u;
    sequence_live_ = true;
}

void SfxMixer::advance_sequence() {
    if (!sequence_live_) {
        return;
    }
    if (sequence_tag_ == 0) {
        if (sequence_wait_frames_ > 0) {
            --sequence_wait_frames_;
            if (sequence_wait_frames_ > 0) {
                return;
            }
        }
        const SfxId id = sequence_[static_cast<std::size_t>(sequence_pos_)];
        start_event(id);
        sequence_tag_ = static_cast<std::uint32_t>(id) + 1u;
        return;
    }
    if (voice_tag_active(sequence_tag_)) {
        return;
    }
    ++sequence_pos_;
    if (sequence_pos_ >= sequence_count_) {
        sequence_live_ = false;
        sequence_tag_ = 0;
        sequence_wait_frames_ = 0;
        sequence_delay_frames_ = 0;
        return;
    }
    sequence_tag_ = 0;
    sequence_wait_frames_ = sequence_delay_frames_;
    if (sequence_wait_frames_ == 0) {
        const SfxId id = sequence_[static_cast<std::size_t>(sequence_pos_)];
        start_event(id);
        sequence_tag_ = static_cast<std::uint32_t>(id) + 1u;
    }
}

int SfxMixer::spare_voice() const {
    for (int i = 0; i < kSfxVoiceLimit; ++i) {
        if (!voices_[static_cast<std::size_t>(i)].active) {
            return i;
        }
    }
    int quietest = 0;
    for (int i = 1; i < kSfxVoiceLimit; ++i) {
        if (less_valuable(voices_[static_cast<std::size_t>(i)],
                          voices_[static_cast<std::size_t>(quietest)])) {
            quietest = i;
        }
    }
    return quietest;
}

void SfxMixer::start_note(const SfxNote& note) {
    if (note.pcm == nullptr || note.sample_count <= 0) {
        return;
    }
    Voice& voice = voices_[static_cast<std::size_t>(spare_voice())];
    voice = Voice{};
    voice.pcm = note.pcm;
    voice.sample_count = note.sample_count;
    voice.loop = note.loop && note.loop_end > note.loop_start;
    voice.loop_start = note.loop_start;
    voice.loop_end = note.loop_end;
    voice.step_q16 = step_for_rate(note.rate_hz);
    voice.gain_l_q15 = note.gain_l_q15;
    voice.gain_r_q15 = note.gain_r_q15;
    voice.priority = note.priority;
    voice.tag = note.tag;
    voice.age = next_age_++;
    voice.active = true;
    voice.use_adsr = note.use_adsr;
    voice.release_at_end = note.use_adsr && note.release_at_end && !voice.loop;
    const std::uint64_t end_q = static_cast<std::uint64_t>(voice.sample_count) << 16;
    const std::uint64_t fade_q = static_cast<std::uint64_t>(kSfxEndFadeFrames) * voice.step_q16;
    voice.fade_at_end = note.fade_at_end && !voice.loop && end_q > fade_q;
    if (voice.fade_at_end) {
        voice.fade_start_q16 = static_cast<std::uint32_t>(end_q - fade_q);
    }
    voice.ducks_music = note.ducks_music;
    voice.resample = note.resample;
    if (note.use_adsr) {
        adsr_key_on(voice.adsr, note.adsr1, note.adsr2);
    }
}

SfxChoice SfxMixer::resolve(SfxId id) const {
    SfxChoice choice;
    const auto index = static_cast<std::size_t>(id);
    if (index >= map_.size()) {
        return choice;
    }
    const SfxMapEntry& entry = map_[index];
    choice.bank = entry.bank;
    choice.program = entry.program;
    choice.tone = entry.tone;
    choice.vag = entry.vag;
    choice.rate_hz = entry.rate_hz > 0 ? entry.rate_hz : kSfxDefaultRateHz;
    choice.rate_confirmed = entry.rate_confirmed;
    choice.confidence = entry.confidence;
    const bool confident = entry.confidence == SfxConfidence::High ||
                           (config_.experimental && entry.confidence != SfxConfidence::None &&
                            entry.confidence != SfxConfidence::High);
    if (entry.bank == kSfxBankNone || entry.bank >= kSfxBankCount || !confident) {
        return choice;
    }
    if (library_ == nullptr || !library_->banks[entry.bank].loaded) {
        return choice;
    }
    const VabBank& vab = library_->banks[entry.bank].vab;
    if (entry.vag != kSfxVagUnset) {
        if (entry.vag >= vab.vags.size() || vab.vags[entry.vag].pcm.empty()) {
            return choice;
        }
        choice.placeholder = false;
        return choice;
    }
    if (entry.program >= vab.header.program_count || entry.tone >= 16) {
        return choice;
    }
    const std::size_t tone_index = static_cast<std::size_t>(entry.program) * 16u + entry.tone;
    if (tone_index >= vab.header.tones.size()) {
        return choice;
    }
    const VabTone& tone = vab.header.tones[tone_index];
    if (tone.vag >= vab.vags.size() || vab.vags[tone.vag].pcm.empty()) {
        return choice;
    }
    choice.placeholder = false;
    return choice;
}

void SfxMixer::start_event(SfxId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(SfxId::Count)) {
        return;
    }
    const auto tag = static_cast<std::uint32_t>(id) + 1u;
    const auto loop_tag = static_cast<std::uint32_t>(SfxId::MenuLoop) + 1u;
    // Select-follow is the menu-loop sample. Keying it while the loop is
    // already up stacks a second copy of that bed.
    if (id == SfxId::MenuSelectFollow && voice_tag_active(loop_tag)) {
        return;
    }
    // A looping cue replaces its own voice. A second post must not add another.
    if (index < map_.size() && map_[index].loop) {
        silence_tag(tag);
    }
    const SfxChoice choice = resolve(id);
    // The promotion cue is the disc sample only. No bank, no tone.
    if (id == SfxId::SuperPromote && (choice.placeholder || library_ == nullptr)) {
        return;
    }
    if (choice.placeholder || library_ == nullptr) {
        SfxNote note;
        note.pcm = placeholders_[index].data();
        note.sample_count = static_cast<int>(placeholders_[index].size());
        note.rate_hz = kCddaRate;
        note.use_adsr = false;
        note.gain_l_q15 = 20000;
        note.gain_r_q15 = 20000;
        note.resample = config_.resample;
        note.tag = static_cast<std::uint32_t>(id) + 1u;
        start_note(note);
        return;
    }
    const SfxMapEntry& entry = map_[index];
    const VabBank& vab = library_->banks[entry.bank].vab;
    const DecodedVag* decoded = nullptr;
    const VabTone* tone = nullptr;
    const VabProgram* program = nullptr;
    if (entry.vag != kSfxVagUnset) {
        if (entry.vag < vab.vags.size()) {
            decoded = &vab.vags[entry.vag];
        }
        for (std::size_t i = 0; i < vab.header.tones.size(); ++i) {
            if (vab.header.tones[i].vag != entry.vag) {
                continue;
            }
            tone = &vab.header.tones[i];
            const std::size_t program_index = i / 16u;
            if (program_index < vab.header.programs.size()) {
                program = &vab.header.programs[program_index];
            }
            break;
        }
    } else {
        const std::size_t tone_index = static_cast<std::size_t>(entry.program) * 16u + entry.tone;
        if (tone_index < vab.header.tones.size()) {
            tone = &vab.header.tones[tone_index];
            if (tone->vag < vab.vags.size()) {
                decoded = &vab.vags[tone->vag];
            }
            if (entry.program < vab.header.programs.size()) {
                program = &vab.header.programs[entry.program];
            }
        }
    }
    if (decoded == nullptr || decoded->pcm.empty()) {
        if (id == SfxId::SuperPromote) {
            return;
        }
        SfxNote note;
        note.pcm = placeholders_[index].data();
        note.sample_count = static_cast<int>(placeholders_[index].size());
        note.rate_hz = kCddaRate;
        note.use_adsr = false;
        note.gain_l_q15 = 20000;
        note.gain_r_q15 = 20000;
        note.resample = config_.resample;
        note.tag = static_cast<std::uint32_t>(id) + 1u;
        start_note(note);
        return;
    }
    const int program_volume = program != nullptr ? program->volume : 127;
    const int tone_volume = tone != nullptr ? tone->volume : 127;
    const int pan = tone != nullptr ? tone->pan : 64;
    SfxNote note;
    note.pcm = decoded->pcm.data();
    note.sample_count = static_cast<int>(decoded->pcm.size());
    note.rate_hz = choice.rate_hz;
    note.use_adsr = tone != nullptr;
    note.resample = config_.resample;
    note.fade_at_end = true;
    if (tone != nullptr) {
        note.adsr1 = tone->adsr1;
        note.adsr2 = tone->adsr2;
        note.priority = tone->priority;
    }
    note.gain_l_q15 = channel_gain(
        vab.header.master_volume, program_volume, tone_volume, pan, true, config_.master_q15);
    note.gain_r_q15 = channel_gain(
        vab.header.master_volume, program_volume, tone_volume, pan, false, config_.master_q15);
    note.tag = static_cast<std::uint32_t>(id) + 1u;
    note.ducks_music = id != SfxId::SuperPromote;
    if (entry.loop && decoded->loop.has_end && decoded->loop.repeat &&
        decoded->loop.end_sample > decoded->loop.start_sample &&
        decoded->loop.end_sample <= note.sample_count) {
        note.loop = true;
        note.loop_start = decoded->loop.has_start ? decoded->loop.start_sample : 0;
        note.loop_end = decoded->loop.end_sample;
        if (note.loop_start >= note.loop_end) {
            note.loop_start = 0;
        }
    }
    // An end flag without repeat forces the voice off. No end flag releases.
    note.release_at_end = !(decoded->loop.has_end && !decoded->loop.repeat);
    start_note(note);
}

void SfxMixer::apply_commands() {
    Command local[kQueue];
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        count = queue_count_;
        for (int i = 0; i < count; ++i) {
            local[i] = queue_[static_cast<std::size_t>((queue_read_ + i) % kQueue)];
        }
        queue_count_ = 0;
        queue_read_ = 0;
    }
    for (int i = 0; i < count; ++i) {
        if (local[i].kind == Command::Kind::Note) {
            start_note(local[i].note);
        } else if (local[i].kind == Command::Kind::Sequence) {
            begin_sequence(local[i].sequence, local[i].sequence_count, local[i].sequence_delay_ms);
        } else if (local[i].kind == Command::Kind::Stop) {
            silence_tag(static_cast<std::uint32_t>(local[i].id) + 1u);
        } else if (local[i].kind == Command::Kind::StopTag) {
            silence_tag(local[i].tag);
        } else {
            start_event(local[i].id);
        }
    }
}

void SfxMixer::drain() {
    apply_commands();
}

void SfxMixer::mix(std::span<std::int16_t> interleaved, int frames) {
    apply_commands();

    const bool sounding = std::any_of(
        voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.active; });
    if (!sounding && !sequence_live_ && music_gain_ == kEnvMax) {
        published_gain_.store(kEnvMax, std::memory_order_relaxed);
        return;
    }

    if (frames < 0) {
        frames = 0;
    }
    const int available = static_cast<int>(interleaved.size() / 2u);
    if (frames > available) {
        frames = available;
    }
    for (int frame = 0; frame < frames; ++frame) {
        std::int32_t acc_l = 0;
        std::int32_t acc_r = 0;
        bool ducking = false;
        for (Voice& voice : voices_) {
            if (!voice.active) {
                continue;
            }
            if (voice.ducks_music) {
                ducking = true;
            }
            if (voice.use_adsr) {
                adsr_tick(voice.adsr);
                if (voice.adsr.phase == 4) {
                    voice.active = false;
                    continue;
                }
            }
            const int raw = interpolate(voice);
            const int level = voice.use_adsr ? voice.adsr.level : kEnvMax;
            // 32767 is unity. Multiplying by it is not bit-identical.
            std::int32_t shaped = raw;
            if (level != kEnvMax) {
                shaped = static_cast<std::int32_t>((static_cast<std::int64_t>(raw) * level) >> 15);
            }
            if (voice.fade_at_end) {
                const std::uint64_t end_q = static_cast<std::uint64_t>(voice.sample_count) << 16;
                const std::uint64_t fade_q = end_q - voice.fade_start_q16;
                const std::uint64_t remaining_q =
                    voice.pos_q16 >= end_q ? 0 : end_q - voice.pos_q16;
                const int fade_gain = static_cast<int>(
                    std::min<std::uint64_t>(kEnvMax, remaining_q * kEnvMax / fade_q));
                shaped = static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(shaped) * fade_gain) >> 15);
            }
            const auto scale = [](std::int32_t sample, int gain) {
                if (gain == kEnvMax) {
                    return sample;
                }
                return static_cast<std::int32_t>((static_cast<std::int64_t>(sample) * gain) >> 15);
            };
            acc_l += scale(shaped, voice.gain_l_q15);
            acc_r += scale(shaped, voice.gain_r_q15);
            advance_voice(voice);
        }
        // User SFX volume. Unity skips the multiply so a default mix stays identical.
        const int sfx_gain = sfx_gain_.load(std::memory_order_relaxed);
        if (sfx_gain != kEnvMax) {
            acc_l = static_cast<std::int32_t>((static_cast<std::int64_t>(acc_l) * sfx_gain) >> 15);
            acc_r = static_cast<std::int32_t>((static_cast<std::int64_t>(acc_r) * sfx_gain) >> 15);
        }
        std::int32_t left = interleaved[static_cast<std::size_t>(frame) * 2u];
        std::int32_t right = interleaved[static_cast<std::size_t>(frame) * 2u + 1u];
        if (music_gain_ != kEnvMax) {
            left = static_cast<std::int32_t>((static_cast<std::int64_t>(left) * music_gain_) >> 15);
            right =
                static_cast<std::int32_t>((static_cast<std::int64_t>(right) * music_gain_) >> 15);
        }
        interleaved[static_cast<std::size_t>(frame) * 2u] =
            static_cast<std::int16_t>(sat16(left + acc_l));
        interleaved[static_cast<std::size_t>(frame) * 2u + 1u] =
            static_cast<std::int16_t>(sat16(right + acc_r));
        step_gain(music_gain_, ducking ? kDuckGain : kEnvMax);
        advance_sequence();
    }
    published_gain_.store(music_gain_, std::memory_order_relaxed);
}

int SfxMixer::music_gain_q15() const {
    return published_gain_.load(std::memory_order_relaxed);
}

int SfxMixer::active_voices() const {
    int count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active) {
            ++count;
        }
    }
    return count;
}

int SfxMixer::active_with_tag(std::uint32_t tag) const {
    if (tag == 0) {
        return 0;
    }
    int count = 0;
    for (const Voice& voice : voices_) {
        if (voice.active && voice.tag == tag) {
            ++count;
        }
    }
    return count;
}

bool ensure_menu_loop(SfxMixer& mixer) {
    // A paused device may not have mixed the stop from entering play. Count
    // the voices after that stop, or a later mix silences the bed we kept.
    mixer.drain();
    const auto tag = static_cast<std::uint32_t>(SfxId::MenuLoop) + 1u;
    const int playing = mixer.active_with_tag(tag);
    if (mixer.resolve(SfxId::MenuLoop).placeholder) {
        if (playing > 1) {
            mixer.stop(SfxId::MenuLoop);
        }
        return false;
    }
    if (playing == 1) {
        return false;
    }
    mixer.stop(SfxId::MenuLoop);
    mixer.post(SfxId::MenuLoop);
    return true;
}

bool SfxMixer::voice_tag_active(std::uint32_t tag) const {
    for (const Voice& voice : voices_) {
        if (voice.active && voice.tag == tag) {
            return true;
        }
    }
    return false;
}

bool SfxMixer::sequence_active() const {
    return sequence_live_;
}

bool SfxMixer::pending() const {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return queue_count_ > 0;
}

} // namespace oscilline
