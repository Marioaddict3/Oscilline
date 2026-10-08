// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Title and language menu models, clips, and archive names.

#pragma once

#include "oscilline/anm.hpp"
#include "oscilline/asset/map.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/tmd.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// Title pages. Music and the disc row have no disc model; their labels stay
// on the font. Language rows use the selected language archive's label model
// when that slot resolves.
enum class MenuPage : std::uint8_t { Main, Wheel, Scores, Music, Language, Options, About };

// The on-disc model at `TITLE/VIBRI` and its clips stay mapped at high confidence.
// Menus omit the model while this is false. Menu labels, the wheel, and other
// menu art do not read this.
inline constexpr bool kShowMenuCharacter = false;

enum class MenuMotion : std::uint8_t { Idle, Rotate, Open, Close };

enum class MenuAction : std::uint8_t { None, Move, Confirm, Back, Adjust };

enum class MenuIntent : std::uint8_t {
    None,
    Quit,
    OpenDisc,
    OpenMusic,
    LaunchCourse,
    LaunchMusic,
    SetLanguage,
    ToggleOption,
    BeginCalibration,
    ResetSettings,
    ResetHighScores,
    // Confirm on a Controls action row. The next key or pad button binds it.
    BeginRebind,
    ResetControls,
};

// Rows when Play Original is shown. A hidden Play row shifts the later indices;
// use main_menu_item() for the list that is on screen.
inline constexpr int kMenuMainPlay = 0;
inline constexpr int kMenuMainDisc = 1;
inline constexpr int kMenuMainMusic = 2;
inline constexpr int kMenuMainOptions = 3;
inline constexpr int kMenuMainAbout = 4;
inline constexpr int kMenuMainQuit = 5;
inline constexpr int kMenuMainCount = 6;

enum class MainItem : std::uint8_t { Play, Disc, Music, Options, About, Quit };

// How many title rows are on screen. Play Original needs a playable disc;
// Load Disc is omitted after a supported disc is loaded.
[[nodiscard]] int main_menu_count(bool play_available, bool disc_loaded = false);

[[nodiscard]] MainItem main_menu_item(int index, bool play_available, bool disc_loaded = false);

// Index of `item`, or 0 when that row is hidden.
[[nodiscard]] int main_menu_index(MainItem item, bool play_available, bool disc_loaded = false);

[[nodiscard]] const char* main_menu_label(MainItem item);

// Options settings remain identified by setting id; the visible list groups them by category.
// Up and down stop at the ends, the same as the title list.
inline constexpr int kOptionsRibbon = 0;
inline constexpr int kOptionsMusic = 1;
inline constexpr int kOptionsSfx = 2;
inline constexpr int kOptionsTiming = 3;
inline constexpr int kOptionsCamera = 4;
inline constexpr int kOptionsDisc = 5;
inline constexpr int kOptionsFullscreen = 6;
inline constexpr int kOptionsWindow = 7;
// Disc and menu text shake. Left and right step the saved intensity.
inline constexpr int kOptionsTextShake = 8;
inline constexpr int kOptionsDiscAssets = 9;
inline constexpr int kOptionsLanguage = 10;
inline constexpr int kOptionsAspect = 11;
inline constexpr int kOptionsBack = 12;
inline constexpr int kOptionsReset = 13;
inline constexpr int kOptionsResetHighScores = 14;
// Interface: coupons or a number, the PERFECT/GOOD/MISS text, and the
// in-level control line.
inline constexpr int kOptionsScoreDisplay = 15;
inline constexpr int kOptionsControlHints = 16;
// Controls: one row per GameAction, in GameAction order, then a reset.
inline constexpr int kOptionsBindBlock = 17;
inline constexpr int kOptionsBindLoop = 18;
inline constexpr int kOptionsBindWave = 19;
inline constexpr int kOptionsBindPit = 20;
inline constexpr int kOptionsResetControls = 21;
inline constexpr int kOptionsTimingHints = 22;
// Video: HD or the classic 286-line frame.
inline constexpr int kOptionsResolution = 23;
inline constexpr int kOptionsVideoHeader = 100;
inline constexpr int kOptionsAudioHeader = 101;
inline constexpr int kOptionsInterfaceHeader = 102;
inline constexpr int kOptionsOtherHeader = 103;
inline constexpr int kOptionsControlsHeader = 104;
inline constexpr int kOptionsCount = 27;
inline constexpr int kOptionsCategoryVideo = 0;
inline constexpr int kOptionsCategoryAudio = 1;
inline constexpr int kOptionsCategoryInterface = 2;
inline constexpr int kOptionsCategoryControls = 3;
inline constexpr int kOptionsCategoryOther = 4;
inline constexpr int kOptionsCategoryCount = 5;
// The GameAction index of a Controls binding row, or -1.
[[nodiscard]] int options_bind_action(int setting_row);
// Left and right change this row's value.
[[nodiscard]] bool options_row_adjustable(int setting_row);
[[nodiscard]] const char* options_category_name(int category);
[[nodiscard]] int options_category_row_count(int category);
[[nodiscard]] int options_category_setting_row(int category, int row);
[[nodiscard]] int options_setting_row(int menu_index);
[[nodiscard]] int options_menu_index(int setting_row);
inline constexpr int kMenuMusicChoose = 3;
inline constexpr int kMenuMusicBack = 4;
inline constexpr int kMenuMusicCount = 5;

inline constexpr float kMenuRotateSeconds = 0.28f;
inline constexpr float kMenuOpenSeconds = 0.35f;

struct MenuClock {
    MenuPage page = MenuPage::Main;
    int index = 0;
    MenuMotion motion = MenuMotion::Idle;
    float motion_u = 0.f;
    double idle_seconds = 0;
    MenuPage pending_page = MenuPage::Main;
    int pending_index = 0;
    bool open_after = false;
    int options_category = 0;
    bool options_tabs_focused = false;
};

struct MenuInput {
    int delta = 0;
    bool confirm = false;
    bool back = false;
    // Left is -1 and right is +1. Vertical lists, including scores, ignore this.
    int horizontal = 0;
};

struct MenuStep {
    MenuAction action = MenuAction::None;
    MenuIntent intent = MenuIntent::None;
    int language = -1;
    int wheel_row = -1;
    int music_row = -1;
    // Options row touched by left/right or confirm. -1 when the step is not that.
    int option_row = -1;
    // Left is -1 and right is +1 on an options value. 0 on a confirm.
    int adjust = 0;
};

// Indices into one model's animation list. -1 is absent. A row below
// `minimum` stays unset. Every menu clip stem is high, so idle, rotate, open,
// and close play at the default gate.
struct MenuClipSet {
    int idle = -1;
    int rotate = -1;
    int open_clip = -1;
    int close_clip = -1;
};

struct MenuVisual {
    int clip = -1;
    float fraction = 0.f;
    bool loop = true;
    double seconds = 0;
};

enum class MenuClipRole : std::uint8_t { Idle, Rotate, Open, Close };

struct MenuClipToken {
    const char* text = "";
    MenuClipRole role = MenuClipRole::Idle;
    Confidence confidence = Confidence::Low;
    Evidence evidence = Evidence::Name;
    const char* evidence_note = "";
};

struct LanguageRow {
    // Our label, not a string from the disc.
    const char* label = "";
    const char* pak = "";
};

[[nodiscard]] std::span<const MenuClipToken> menu_clip_tokens();

[[nodiscard]] std::span<const LanguageRow> language_rows();

// english, japanese, german, spanish, french, or italian. Empty when unknown.
[[nodiscard]] std::optional<std::string> language_pak_for(std::string_view name);

[[nodiscard]] int
menu_page_count(MenuPage page, bool play_available = true, bool disc_loaded = false);

// Custom music, the disc picker, scores, and the wheel's scores/back rows
// have no disc counterpart. Their labels stay on the font.
[[nodiscard]] bool menu_row_placeholder(MenuPage page, int index);

[[nodiscard]] bool menu_page_uses_model(MenuPage page);

[[nodiscard]] int menu_shown_index(const MenuClock& clock);

[[nodiscard]] MenuClipSet menu_clips(const std::vector<std::string>& names, Confidence minimum);

[[nodiscard]] MenuVisual menu_visual(const MenuClock& clock, const MenuClipSet& clips);

// `music_ready` sends the main music row to the music page. Otherwise that
// row asks for a file. `play_available` shows Play Original; `disc_loaded`
// hides Load Disc. `language_row`
// is the current archive in language_rows(); confirm on Language opens that
// row, and left or right cycles it. Input during a clip is ignored.
[[nodiscard]] MenuStep menu_step(MenuClock& clock,
                                 MenuInput input,
                                 const MenuClipSet& clips,
                                 float dt_seconds,
                                 bool music_ready = false,
                                 bool play_available = true,
                                 int language_row = 1,
                                 bool disc_loaded = false);

// Side-view fit of a menu model into `rect`. A missing clip draws the bind pose.
// `cull_profile_eyes` opts into unfinished far-eye culling. It is disabled by
// default; the menu look is the +X side view, so that is the eye on model -X.
[[nodiscard]] std::vector<Segment> menu_model_frame(const TmdModel& model,
                                                    const std::vector<AnmFile>* anims,
                                                    const MenuVisual& visual,
                                                    ScreenRect rect,
                                                    bool cull_profile_eyes = false);

} // namespace oscilline
