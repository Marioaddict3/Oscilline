// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Event-cue map and the VH/VB voice mixer.

#pragma once

#include "oscilline/course/obstacle.hpp"
#include "oscilline/vab.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// Banks on the disc. GAME/AUDIO is the game kit. When that folder contains
// PSJ_SE, that pair is the game bank: the wheels play from it. TITLE/AUDIO is
// the title kit and KIOSK/AUDIO the tutorial kit. An asset registry can fill
// SfxLibrary from its own slots later; the mixer never opens a file itself.
inline constexpr std::uint8_t kSfxBankGame = 0;
inline constexpr std::uint8_t kSfxBankTitle = 1;
inline constexpr std::uint8_t kSfxBankTutorial = 2;
inline constexpr std::uint8_t kSfxBankCount = 3;
inline constexpr std::uint8_t kSfxBankNone = 0xFF;

// SPU pitch 0x03A0 at the 44100 Hz output clock: 44100 * 0x03A0 / 4096.
inline constexpr int kSfxPitch9991Hz = 9991;

// A map row with this VAG value plays the program/tone sample. Any other
// value is the zero-based slot in that bank's VAG table.
inline constexpr std::uint16_t kSfxVagUnset = 0xFFFF;

// Longest announcement the mixer will chain. The captured lines are shorter.
inline constexpr int kSfxSequenceMax = 4;

// How many voices the mixer will sound at once. One more note steals the
// quietest, lowest-priority voice.
inline constexpr int kSfxVoiceLimit = 16;

// Default rate for entries whose playback rate is not yet confirmed.
inline constexpr int kSfxDefaultRateHz = 11025;

enum class SfxConfidence : std::uint8_t { None, Low, Medium, High };

enum class SfxResample : std::uint8_t { Linear, Gaussian4 };

// Menu sounds, one clear and one miss for each obstacle kind, then course
// lifecycle cues. Obstacle rows are `Cleared + kind` and `Missed + kind`.
// Rows after LevelComplete are later key-on matches. They are appended so the
// success, miss, and lifecycle indices stay put. Announcement parts are one
// row each so a line can play them in order.
enum class SfxId : std::uint16_t {
    MenuMove = 0,
    MenuSelect,
    MenuBack,
    MenuLoop,
    Cleared,
    Missed = Cleared + kObstacleKindCount,
    FormChange = Missed + kObstacleKindCount,
    RoundStart,
    GameOver,
    LevelComplete,
    Whiff,
    WhiffAlt,
    PostCrashPress,
    MenuSelectFollow,
    DifficultyConfirm,
    DifficultyConfirmFollow,
    TitleConfirm,
    AnnounceDisc0,
    AnnounceDisc1,
    AnnounceJoin,
    AnnounceCd0,
    AnnounceCd1,
    AnnounceOptions,
    AnnounceBack,
    AnnounceBronze,
    AnnounceSilver,
    AnnounceGold,
    AnnounceScores,
    // Rabbit → super on the 18th clear. Appended so earlier indices stay put.
    SuperPromote,
    // TITLE VAG 17 (program 9 tone 7). Plays once when the round-start voice ends.
    MusicStart,
    Count
};

[[nodiscard]] inline SfxId sfx_cleared(int kind) {
    return static_cast<SfxId>(static_cast<int>(SfxId::Cleared) + kind);
}

[[nodiscard]] inline SfxId sfx_missed(int kind) {
    return static_cast<SfxId>(static_cast<int>(SfxId::Missed) + kind);
}

// One row of the event map. A row names a bank plus a program and tone, or a
// bank plus a VAG index when the capture names the sample slot. The evidence
// note names how the row was chosen (name, format, or emulator). It does not
// cite code. A low or medium row plays the placeholder unless the experimental
// switch is on. An unmapped bank always plays the placeholder. Gameplay posts
// a mapped medium row only when that switch resolves a sample.
struct SfxMapEntry {
    SfxId id = SfxId::MenuMove;
    std::uint8_t bank = kSfxBankNone;
    std::uint8_t program = 0;
    std::uint8_t tone = 0;
    int rate_hz = kSfxDefaultRateHz;
    bool rate_confirmed = false;
    SfxConfidence confidence = SfxConfidence::Low;
    const char* evidence = "";
    // Most event cues key off as one-shots even when their VAG has loop flags.
    // Keep only cues observed as sustained playback on the loop path.
    bool loop = false;
    std::uint16_t vag = kSfxVagUnset;
};

// The shipped event map. Rows with unresolved capture matches stay unmapped.
extern const SfxMapEntry kSfxMap[static_cast<std::size_t>(SfxId::Count)];

struct SfxConfig {
    bool experimental = false;
    SfxResample resample = SfxResample::Linear;
    // Q15. 32767 is unity. Scales voices, not the music.
    int master_q15 = 32767;
};

// Decoded banks the mixer can play. `load_sfx_library` fills this from a
// mounted disc. A later registry can build the same struct and skip the loader.
struct SfxLibrary {
    struct Bank {
        bool loaded = false;
        VabBank vab;
        std::string path;
    };
    Bank banks[kSfxBankCount];
};

class IsoVolume;

// Reads the VH/VB pair in GAME/AUDIO, TITLE/AUDIO, and KIOSK/AUDIO. GAME prefers
// the PSJ_SE stem, TITLE the TITLE stem, and KIOSK the KIOSK stem. A folder
// without that stem keeps the first pair. A missing pair leaves that bank
// empty. A pair that fails to parse is skipped with a warning. Disc bytes are
// not copied anywhere except this process.
[[nodiscard]] SfxLibrary load_sfx_library(const IsoVolume& volume);

[[nodiscard]] std::string_view sfx_name(SfxId id);

// A decoded VAG in a loaded bank that no kSfxMap row plays. `vag` is the
// zero-based slot. `has_tone` names the first live program and tone that
// points at that slot, when one does. The event map itself is not changed.
struct SfxBankSample {
    std::uint8_t bank = kSfxBankNone;
    std::uint16_t vag = 0;
    bool has_tone = false;
    std::uint8_t program = 0;
    std::uint8_t tone = 0;
};

[[nodiscard]] std::vector<SfxBankSample> unidentified_sfx(const SfxLibrary& library);

// A note the mixer can start directly. Tests use this; the game posts SfxIds.
struct SfxNote {
    const std::int16_t* pcm = nullptr;
    int sample_count = 0;
    int rate_hz = kSfxDefaultRateHz;
    bool loop = false;
    int loop_start = 0;
    int loop_end = 0;
    int gain_l_q15 = 32767;
    int gain_r_q15 = 32767;
    std::uint16_t adsr1 = 0;
    std::uint16_t adsr2 = 0;
    // When false, the note holds full level until the sample ends.
    bool use_adsr = true;
    // When set, a one-shot note enters release at the last sample. An ADPCM
    // end flag without the repeat bit stops instead, which is the documented
    // force-release.
    bool release_at_end = true;
    // A short output fade avoids a click at the end of an imported one-shot.
    bool fade_at_end = false;
    int priority = 64;
    std::uint32_t tag = 0;
    SfxResample resample = SfxResample::Linear;
    // The rabbit → super cue is mixed without pulling the music down.
    bool ducks_music = true;
};

// Fills `note` from one bank sample. The PCM pointer stays inside `library`.
// The rate is kSfxDefaultRateHz. A VAG with a repeat loop is marked to loop.
// Returns false when the bank is missing or that sample is empty.
[[nodiscard]] bool
sfx_bank_note(const SfxLibrary& library, std::uint8_t bank, std::uint16_t vag, SfxNote& note);

struct SfxChoice {
    bool placeholder = true;
    std::uint8_t bank = kSfxBankNone;
    std::uint8_t program = 0;
    std::uint8_t tone = 0;
    std::uint16_t vag = kSfxVagUnset;
    int rate_hz = kSfxDefaultRateHz;
    bool rate_confirmed = false;
    SfxConfidence confidence = SfxConfidence::None;
};

// SPU ADSR, stepped once per output sample. Levels are 0..32767.
// The bit fields and the counter follow psx-spx "SPU Volume and ADSR Generator".
struct AdsrState {
    std::uint16_t adsr1 = 0;
    std::uint16_t adsr2 = 0;
    int level = 0;
    int cycles = 1;
    int amount = 0;
    int wait = 0;
    // 0 attack, 1 decay, 2 sustain, 3 release, 4 off.
    int phase = 4;
    int shift = 0;
    // Signed step before the shift is applied: +7..+4 or -8..-5.
    int base_step = 0;
    bool exponential = false;
    bool increase = true;
    int sustain_level = 0;
};

void adsr_key_on(AdsrState& state, std::uint16_t adsr1, std::uint16_t adsr2);
void adsr_key_off(AdsrState& state);
// One step at the output rate. Returns the level after the step.
int adsr_tick(AdsrState& state);

// Mixes voices onto 44.1 kHz stereo. `post` is safe from another thread.
// `mix` is the single consumer: it drains the queue, ducks the buffer, and
// adds voices. Music samples are left unchanged when nothing is sounding and
// the duck gain is unity.
class SfxMixer {
  public:
    SfxMixer(const SfxLibrary* library, std::span<const SfxMapEntry> map, SfxConfig config);

    SfxMixer(const SfxMixer&) = delete;
    SfxMixer& operator=(const SfxMixer&) = delete;

    void set_library(const SfxLibrary* library);
    void set_experimental(bool experimental);
    // Q15 user volume on every voice, applied while mixing so a loop follows the slider.
    // 32767 is unity and leaves the samples unchanged.
    void set_sfx_gain_q15(int gain_q15);

    void post(SfxId id);
    // Releases every voice of this cue. A looping cue must be stopped before
    // another start, or the two copies play together.
    void stop(SfxId id);
    // Releases every voice with this tag. Tags used by SfxId posts are
    // `id + 1`. A direct note can use any other non-zero tag.
    void stop_tag(std::uint32_t tag);
    void post_note(const SfxNote& note);
    // Plays `ids` one after another, with an optional gap after each voice.
    // A new sequence cuts the one already playing. Extra ids past
    // kSfxSequenceMax are dropped.
    void post_sequence(std::span<const SfxId> ids, int inter_part_delay_ms = 0);

    // `interleaved` is stereo, `frames` pairs. Extra samples past `frames` are
    // left alone. A short span is clamped.
    void mix(std::span<std::int16_t> interleaved, int frames);
    // Applies queued notes and stops without rendering. A paused output does
    // not mix, so a stop can sit in the queue while the voice still counts.
    void drain();

    [[nodiscard]] int music_gain_q15() const;
    [[nodiscard]] int active_voices() const;
    [[nodiscard]] bool voice_tag_active(std::uint32_t tag) const;
    [[nodiscard]] int active_with_tag(std::uint32_t tag) const;
    [[nodiscard]] bool pending() const;
    [[nodiscard]] bool sequence_active() const;
    [[nodiscard]] SfxChoice resolve(SfxId id) const;

  private:
    struct Voice {
        const std::int16_t* pcm = nullptr;
        int sample_count = 0;
        bool loop = false;
        int loop_start = 0;
        int loop_end = 0;
        std::uint32_t pos_q16 = 0;
        std::uint32_t step_q16 = 0;
        int gain_l_q15 = 0;
        int gain_r_q15 = 0;
        int priority = 0;
        std::uint32_t tag = 0;
        std::uint32_t age = 0;
        bool active = false;
        bool use_adsr = false;
        bool release_at_end = false;
        bool fade_at_end = false;
        std::uint32_t fade_start_q16 = 0;
        bool ducks_music = true;
        SfxResample resample = SfxResample::Linear;
        AdsrState adsr;
    };

    struct Command {
        enum class Kind : std::uint8_t { Event, Note, Sequence, Stop, StopTag };
        Kind kind = Kind::Event;
        SfxId id = SfxId::MenuMove;
        std::uint32_t tag = 0;
        SfxNote note;
        SfxId sequence[kSfxSequenceMax]{};
        int sequence_count = 0;
        int sequence_delay_ms = 0;
    };

    void start_note(const SfxNote& note);
    void start_event(SfxId id);
    void begin_sequence(const SfxId* ids, int count, int inter_part_delay_ms);
    void advance_sequence();
    void silence_tag(std::uint32_t tag);
    [[nodiscard]] int spare_voice() const;
    void enqueue(const Command& command);
    void apply_commands();
    [[nodiscard]] static int sample_at(const Voice& voice, int index);
    [[nodiscard]] static int interpolate(const Voice& voice);
    static void advance_voice(Voice& voice);
    [[nodiscard]] static bool less_valuable(const Voice& left, const Voice& right);

    const SfxLibrary* library_ = nullptr;
    std::vector<SfxMapEntry> map_;
    SfxConfig config_;
    std::vector<std::int16_t> placeholders_[static_cast<std::size_t>(SfxId::Count)];
    std::vector<Voice> voices_;
    std::uint32_t next_age_ = 1;
    int music_gain_ = 32767;
    std::atomic<int> published_gain_{32767};
    std::atomic<int> sfx_gain_{32767};
    SfxId sequence_[kSfxSequenceMax]{};
    int sequence_count_ = 0;
    int sequence_pos_ = 0;
    int sequence_delay_frames_ = 0;
    int sequence_wait_frames_ = 0;
    bool sequence_live_ = false;
    std::uint32_t sequence_tag_ = 0;

    // Fixed ring shared by `post` and `mix`.
    static constexpr int kQueue = 64;
    mutable std::mutex queue_mu_;
    std::vector<Command> queue_;
    int queue_read_ = 0;
    int queue_count_ = 0;
};

// One menu-loop voice when the disc sample resolves. Pending stops are applied
// first, so a stop that has not been mixed is not treated as a live loop.
// A single loop already playing is left running. If that voice was stopped, or
// more than one is up, leftovers are released and one voice is posted. Returns
// true when that fresh voice was posted. With no disc sample, extras are
// released and nothing is started.
[[nodiscard]] bool ensure_menu_loop(SfxMixer& mixer);

} // namespace oscilline
