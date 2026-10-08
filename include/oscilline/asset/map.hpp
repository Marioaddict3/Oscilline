// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Disc slot map: paths, kinds, confidence, and evidence.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace oscilline {

// One row of the asset map. Every disc slot and every sound event is listed
// here, with the evidence that pointed at it. Low and medium rows stay on the
// placeholder until a local check raises them.
enum class Slot : std::uint8_t {
    FormBase,
    FormDegradedA,
    FormDegradedB,
    FormUpgraded,
    Font,
    RankMarks,
    ScoreCoupons,
    MeterProgress,
    MeterEvolution,
    MeterSpeed,
    RoundCaption,
    ResultsDigits,
    MenuModel,
    MenuWheel,
    MenuLabels,
    CameraRoad,
    CameraIntro,
    CameraPlay,
    CameraS02,
    CameraTvBb,
    CameraTvBs,
    CameraMenu,
    SoundBankGame,
    SoundBankTitle,
    SoundBankTutorial,
    SoundMenuMove,
    SoundConfirm,
    SoundBack,
    SoundBlock,
    SoundLoop,
    SoundWave,
    SoundPit,
    SoundPerfect,
    SoundGood,
    SoundMiss,
    SoundDamage,
    SoundForm,
    SoundTally,
    SoundRound,
    SoundAmbient,
    Count,
};

enum class AssetKind : std::uint8_t {
    Model,
    Font,
    MenuModel,
    Meter,
    CameraPath,
    SoundBank,
    Sound,
};

enum class Confidence : std::uint8_t { Low = 0, Medium = 1, High = 2 };

// Visual: checked by eye against the original game, not by emulator capture.
enum class Evidence : std::uint8_t { Name, Format, Emulator, Visual };

enum class AssetArea : std::uint8_t {
    GamePak,
    TitlePak,
    GameAudio,
    TitleAudio,
    TutorialAudio,
    None,
};

enum class AssetPick : std::uint8_t {
    None,
    Exact,
    LanguageFont,
    FolderModel,
    FolderPair,
    // pattern is a directory, alt is a suffix such as ".ANC".
    FolderSuffix,
    // Any .tmd in the folder, preferring MODEL.TMD, plus every .anm beside it.
    FolderTmd,
    AudioStem,
};

// Rows at or above this confidence are used when the file is present.
// Medium and low stay on the placeholder.
inline constexpr Confidence kUsableConfidence = Confidence::High;

struct MapRow {
    Slot slot = Slot::Font;
    const char* name = "";
    AssetKind kind = AssetKind::Model;
    AssetArea area = AssetArea::None;
    AssetPick pick = AssetPick::None;
    // Path pattern inside the archive, or an ISO path stem for a sound bank.
    // LanguageFont uses pattern for the European file and alt for Japanese.
    const char* pattern = "";
    const char* alt = "";
    // FolderPair: 0 is the clip with the most frames, then 1, then 2.
    int pick_index = 0;
    int program = -1;
    int note = -1;
    Confidence confidence = Confidence::Low;
    Evidence evidence = Evidence::Name;
    // Short evidence note. Names the evidence type. No asset bytes.
    const char* evidence_note = "";
};

[[nodiscard]] std::string_view evidence_name(Evidence evidence);

[[nodiscard]] std::span<const MapRow> asset_map();

[[nodiscard]] const MapRow* map_row(Slot slot);

// Character clip names inside a form folder. The stem is the file name with
// the extension removed. A leading N + digits + separator is an index, and a
// trailing _F is a whiff marker; both are ignored for the role. Remaining
// separators are ignored too. Every token is high. Pair letters are JH
// block+pit, JL block+loop, JW block+wave, HL pit+loop, HW pit+wave, and LW
// loop+wave. A pair keeps the letters' confidence. Trailing _F whiffs, idle
// variants, and a miss row stay high, checked by eye against the
// original. SUPER is not a clip name.
enum class ClipKind : std::uint8_t { Idle, Action, Miss };

struct ClipToken {
    const char* text = "";
    // Action bits. Idle and miss words leave this 0.
    std::uint8_t actions = 0;
    ClipKind kind = ClipKind::Action;
    Confidence confidence = Confidence::Low;
    Evidence evidence = Evidence::Name;
    const char* evidence_note = "";
};

[[nodiscard]] std::span<const ClipToken> character_clip_tokens();

} // namespace oscilline
