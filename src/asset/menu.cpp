// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Menu clip names and the six language archive paths.

#include "oscilline/asset/menu.hpp"

#include "oscilline/asset/character.hpp"
#include "oscilline/course/difficulty.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <span>

namespace oscilline {
namespace {

constexpr MenuClipToken kMenuClips[] = {
    {"ROTATE",
     MenuClipRole::Rotate,
     Confidence::High,
     Evidence::Visual,
     "visual: stem ROTATE is an item-to-item turn"},
    {"CLOSE",
     MenuClipRole::Close,
     Confidence::High,
     Evidence::Visual,
     "visual: stem CLOSE is a menu close"},
    {"OPEN",
     MenuClipRole::Open,
     Confidence::High,
     Evidence::Visual,
     "visual: stem OPEN is a menu open"},
    {"TURN",
     MenuClipRole::Rotate,
     Confidence::High,
     Evidence::Visual,
     "visual: stem TURN is an item-to-item turn"},
    {"NEXT",
     MenuClipRole::Rotate,
     Confidence::High,
     Evidence::Visual,
     "visual: stem NEXT is an item-to-item turn"},
    {"PREV",
     MenuClipRole::Rotate,
     Confidence::High,
     Evidence::Visual,
     "visual: stem PREV is an item-to-item turn"},
    {"IDLE",
     MenuClipRole::Idle,
     Confidence::High,
     Evidence::Name,
     "name: stem IDLE is the menu idle cycle"},
    {"WAIT",
     MenuClipRole::Idle,
     Confidence::High,
     Evidence::Name,
     "name: stem WAIT is the menu idle cycle"},
    {"SPIN",
     MenuClipRole::Rotate,
     Confidence::High,
     Evidence::Visual,
     "visual: stem SPIN is a menu turn"},
};

constexpr LanguageRow kLanguages[] = {
    {"JAPANESE", "GAME/01_FILES.PAK"},
    {"ENGLISH", "GAME/02_FILES.PAK"},
    {"GERMAN", "GAME/04_FILES.PAK"},
    {"SPANISH", "GAME/08_FILES.PAK"},
    {"FRENCH", "GAME/10_FILES.PAK"},
    {"ITALIAN", "GAME/20_FILES.PAK"},
};

std::string menu_clip_key(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    const auto dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        path = path.substr(0, dot);
    }
    std::string stem;
    stem.reserve(path.size());
    for (unsigned char ch : path) {
        stem.push_back(static_cast<char>(std::toupper(ch)));
    }
    if (stem.size() >= 3 && stem[0] == 'N' &&
        std::isdigit(static_cast<unsigned char>(stem[1])) != 0) {
        std::size_t index = 1;
        while (index < stem.size() && std::isdigit(static_cast<unsigned char>(stem[index])) != 0) {
            ++index;
        }
        if (index < stem.size() && (stem[index] == '_' || stem[index] == '-')) {
            stem.erase(0, index + 1);
        }
    }
    if (stem.size() >= 2) {
        const char separator = stem[stem.size() - 2];
        if ((separator == '_' || separator == '-') && stem.back() == 'F') {
            stem.resize(stem.size() - 2);
        }
    }
    std::string key;
    key.reserve(stem.size());
    for (unsigned char ch : stem) {
        if (ch == '_' || ch == '-' || ch == ' ') {
            continue;
        }
        key.push_back(static_cast<char>(ch));
    }
    return key;
}

const MenuClipToken* match_token(std::string_view key) {
    const MenuClipToken* found = nullptr;
    std::size_t best = 0;
    for (const MenuClipToken& token : kMenuClips) {
        const std::size_t length = std::strlen(token.text);
        if (length == 0 || length != key.size() || length < best) {
            continue;
        }
        if (key != token.text) {
            continue;
        }
        found = &token;
        best = length;
    }
    return found;
}

void begin_motion(MenuClock& clock, MenuMotion motion) {
    clock.motion = motion;
    clock.motion_u = 0.f;
}

void go_idle(MenuClock& clock) {
    clock.motion = MenuMotion::Idle;
    clock.motion_u = 0.f;
    clock.open_after = false;
}

} // namespace

std::span<const MenuClipToken> menu_clip_tokens() {
    return kMenuClips;
}

std::span<const LanguageRow> language_rows() {
    return kLanguages;
}

std::optional<std::string> language_pak_for(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    for (const LanguageRow& row : kLanguages) {
        std::string label(row.label);
        for (char& letter : label) {
            letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        }
        if (label == lower) {
            return std::string(row.pak);
        }
    }
    return std::nullopt;
}

namespace {

constexpr MainItem kMainWithPlay[] = {MainItem::Play,
                                      MainItem::Disc,
                                      MainItem::Music,
                                      MainItem::Options,
                                      MainItem::About,
                                      MainItem::Quit};
constexpr MainItem kMainWithoutPlay[] = {
    MainItem::Disc, MainItem::Music, MainItem::Options, MainItem::About, MainItem::Quit};
constexpr MainItem kMainWithPlayAndDisc[] = {
    MainItem::Play, MainItem::Music, MainItem::Options, MainItem::About, MainItem::Quit};
constexpr MainItem kMainWithoutPlayAndDisc[] = {
    MainItem::Music, MainItem::Options, MainItem::About, MainItem::Quit};

std::span<const MainItem> main_rows(bool play_available, bool disc_loaded) {
    if (play_available) {
        return disc_loaded ? std::span<const MainItem>(kMainWithPlayAndDisc)
                           : std::span<const MainItem>(kMainWithPlay);
    }
    return disc_loaded ? std::span<const MainItem>(kMainWithoutPlayAndDisc)
                       : std::span<const MainItem>(kMainWithoutPlay);
}

} // namespace

int main_menu_count(bool play_available, bool disc_loaded) {
    return static_cast<int>(main_rows(play_available, disc_loaded).size());
}

MainItem main_menu_item(int index, bool play_available, bool disc_loaded) {
    const std::span<const MainItem> rows = main_rows(play_available, disc_loaded);
    if (rows.empty()) {
        return MainItem::Quit;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= static_cast<int>(rows.size())) {
        index = static_cast<int>(rows.size()) - 1;
    }
    return rows[static_cast<std::size_t>(index)];
}

int main_menu_index(MainItem item, bool play_available, bool disc_loaded) {
    const std::span<const MainItem> rows = main_rows(play_available, disc_loaded);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[static_cast<std::size_t>(i)] == item) {
            return i;
        }
    }
    return 0;
}

const char* main_menu_label(MainItem item) {
    switch (item) {
    case MainItem::Play:
        return "PLAY ORIGINAL";
    case MainItem::Disc:
        return "LOAD DISC";
    case MainItem::Music:
        return "PLAY MY MUSIC";
    case MainItem::Options:
        return "OPTIONS";
    case MainItem::About:
        return "ABOUT";
    case MainItem::Quit:
        return "QUIT";
    }
    return "";
}

namespace {
constexpr int kOptionMenuRows[kOptionsCount] = {
    kOptionsVideoHeader,    kOptionsAspect,          kOptionsFullscreen,  kOptionsWindow,
    kOptionsResolution,     kOptionsAudioHeader,     kOptionsMusic,       kOptionsSfx,
    kOptionsTiming,         kOptionsInterfaceHeader, kOptionsDisc,        kOptionsDiscAssets,
    kOptionsTextShake,      kOptionsScoreDisplay,    kOptionsTimingHints, kOptionsControlHints,
    kOptionsControlsHeader, kOptionsBindBlock,       kOptionsBindLoop,    kOptionsBindWave,
    kOptionsBindPit,        kOptionsResetControls,   kOptionsOtherHeader, kOptionsRibbon,
    kOptionsReset,          kOptionsResetHighScores, kOptionsBack,
};
}

namespace {
constexpr int kOptionCategoryRows[kOptionsCategoryCount][7] = {
    {kOptionsAspect, kOptionsFullscreen, kOptionsWindow, kOptionsResolution, kOptionsBack, -1, -1},
    {kOptionsMusic, kOptionsSfx, kOptionsTiming, kOptionsBack, -1, -1, -1},
    {kOptionsDisc,
     kOptionsDiscAssets,
     kOptionsTextShake,
     kOptionsScoreDisplay,
     kOptionsTimingHints,
     kOptionsControlHints,
     kOptionsBack},
    {kOptionsBindBlock,
     kOptionsBindLoop,
     kOptionsBindWave,
     kOptionsBindPit,
     kOptionsResetControls,
     kOptionsBack,
     -1},
    {kOptionsRibbon, kOptionsReset, kOptionsResetHighScores, kOptionsBack, -1, -1, -1},
};
constexpr int kOptionCategoryRowCounts[kOptionsCategoryCount] = {5, 4, 7, 6, 4};
constexpr const char* kOptionCategoryNames[kOptionsCategoryCount] = {
    "VIDEO", "AUDIO", "INTERFACE", "CONTROLS", "OTHER"};
} // namespace

int options_bind_action(int setting_row) {
    if (setting_row < kOptionsBindBlock || setting_row > kOptionsBindPit) {
        return -1;
    }
    return setting_row - kOptionsBindBlock;
}

bool options_row_adjustable(int setting_row) {
    return (setting_row >= 0 && setting_row < kOptionsBack) ||
           setting_row == kOptionsScoreDisplay || setting_row == kOptionsTimingHints ||
           setting_row == kOptionsControlHints || setting_row == kOptionsResolution;
}

const char* options_category_name(int category) {
    if (category < 0 || category >= kOptionsCategoryCount) {
        return kOptionCategoryNames[0];
    }
    return kOptionCategoryNames[category];
}

int options_category_row_count(int category) {
    if (category < 0 || category >= kOptionsCategoryCount) {
        return 0;
    }
    return kOptionCategoryRowCounts[category];
}

int options_category_setting_row(int category, int row) {
    if (category < 0 || category >= kOptionsCategoryCount || row < 0 ||
        row >= kOptionCategoryRowCounts[category]) {
        return -1;
    }
    return kOptionCategoryRows[category][row];
}

int options_setting_row(int menu_index) {
    if (menu_index < 0 || menu_index >= kOptionsCount) {
        return -1;
    }
    return kOptionMenuRows[menu_index];
}

int options_menu_index(int setting_row) {
    for (int i = 0; i < kOptionsCount; ++i) {
        if (kOptionMenuRows[i] == setting_row) {
            return i;
        }
    }
    return 0;
}

int menu_page_count(MenuPage page, bool play_available, bool disc_loaded) {
    switch (page) {
    case MenuPage::Main:
        return main_menu_count(play_available, disc_loaded);
    case MenuPage::Wheel:
        return kWheelSlots;
    case MenuPage::Scores:
        // Bronze, silver, gold, then Back. Same length the list painter draws.
        return kDifficultyCount + 1;
    case MenuPage::Music:
        return kMenuMusicCount;
    case MenuPage::Language:
        return static_cast<int>(language_rows().size());
    case MenuPage::Options:
        return kOptionsCount;
    case MenuPage::About:
        return 1;
    }
    return 1;
}

bool menu_row_placeholder(MenuPage page, int index) {
    if (page == MenuPage::Music || page == MenuPage::Scores || page == MenuPage::Options) {
        return true;
    }
    if (page == MenuPage::Main && (index == kMenuMainDisc || index == kMenuMainMusic)) {
        return true;
    }
    if (page == MenuPage::Wheel && (index == kWheelScores || index == kWheelBack)) {
        return true;
    }
    return false;
}

bool menu_page_uses_model(MenuPage page) {
    return page == MenuPage::Main || page == MenuPage::Wheel || page == MenuPage::Language;
}

int menu_shown_index(const MenuClock& clock) {
    if (clock.motion == MenuMotion::Rotate) {
        return clock.pending_index;
    }
    return clock.index;
}

MenuClipSet menu_clips(const std::vector<std::string>& names, Confidence minimum) {
    MenuClipSet clips;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const MenuClipToken* token = match_token(menu_clip_key(names[i]));
        if (token == nullptr || static_cast<int>(token->confidence) < static_cast<int>(minimum)) {
            continue;
        }
        const int index = static_cast<int>(i);
        switch (token->role) {
        case MenuClipRole::Idle:
            if (clips.idle < 0) {
                clips.idle = index;
            }
            break;
        case MenuClipRole::Rotate:
            if (clips.rotate < 0) {
                clips.rotate = index;
            }
            break;
        case MenuClipRole::Open:
            if (clips.open_clip < 0) {
                clips.open_clip = index;
            }
            break;
        case MenuClipRole::Close:
            if (clips.close_clip < 0) {
                clips.close_clip = index;
            }
            break;
        }
    }
    return clips;
}

MenuVisual menu_visual(const MenuClock& clock, const MenuClipSet& clips) {
    MenuVisual visual;
    visual.seconds = clock.idle_seconds;
    visual.loop = true;
    visual.clip = clips.idle;
    if (clock.motion == MenuMotion::Rotate && clips.rotate >= 0) {
        visual.clip = clips.rotate;
        visual.fraction = std::clamp(clock.motion_u, 0.f, 1.f);
        visual.loop = false;
        visual.seconds = 0;
    } else if (clock.motion == MenuMotion::Open && clips.open_clip >= 0) {
        visual.clip = clips.open_clip;
        visual.fraction = std::clamp(clock.motion_u, 0.f, 1.f);
        visual.loop = false;
        visual.seconds = 0;
    } else if (clock.motion == MenuMotion::Close && clips.close_clip >= 0) {
        visual.clip = clips.close_clip;
        visual.fraction = std::clamp(clock.motion_u, 0.f, 1.f);
        visual.loop = false;
        visual.seconds = 0;
    }
    return visual;
}

MenuStep menu_step(MenuClock& clock,
                   MenuInput input,
                   const MenuClipSet& clips,
                   float dt_seconds,
                   bool music_ready,
                   bool play_available,
                   int language_row,
                   bool disc_loaded) {
    MenuStep step;
    if (!std::isfinite(dt_seconds) || dt_seconds < 0.f) {
        dt_seconds = 0.f;
    }

    if (clock.motion != MenuMotion::Idle) {
        const float duration =
            clock.motion == MenuMotion::Rotate ? kMenuRotateSeconds : kMenuOpenSeconds;
        clock.motion_u += duration > 0.f ? dt_seconds / duration : 1.f;
        if (clock.motion_u < 1.f) {
            return step;
        }
        if (clock.motion == MenuMotion::Rotate) {
            clock.index = clock.pending_index;
            go_idle(clock);
        } else if (clock.motion == MenuMotion::Close) {
            clock.page = clock.pending_page;
            clock.index = clock.pending_index;
            clock.idle_seconds = 0;
            const bool open =
                clock.open_after && clips.open_clip >= 0 && menu_page_uses_model(clock.page);
            go_idle(clock);
            if (open) {
                begin_motion(clock, MenuMotion::Open);
            }
        } else {
            go_idle(clock);
        }
        if (clock.motion == MenuMotion::Idle) {
            clock.idle_seconds += static_cast<double>(dt_seconds);
        }
        return step;
    }

    clock.idle_seconds += static_cast<double>(dt_seconds);

    const auto change_page = [&](MenuPage page, int index) {
        const bool from_model = menu_page_uses_model(clock.page) && clips.close_clip >= 0;
        if (from_model) {
            clock.pending_page = page;
            clock.pending_index = index;
            clock.open_after = menu_page_uses_model(page) && clips.open_clip >= 0;
            begin_motion(clock, MenuMotion::Close);
            return;
        }
        clock.page = page;
        clock.index = index;
        clock.idle_seconds = 0;
        if (menu_page_uses_model(page) && clips.open_clip >= 0) {
            begin_motion(clock, MenuMotion::Open);
        }
    };

    // Options has a horizontal category strip above the vertical settings list.
    // Up from the first row focuses the strip; down or confirm returns to settings.
    if (clock.page == MenuPage::Options) {
        const int count = options_category_row_count(clock.options_category);
        if (input.delta != 0) {
            if (clock.options_tabs_focused) {
                if (input.delta > 0) {
                    clock.options_tabs_focused = false;
                    clock.index = 0;
                    step.action = MenuAction::Move;
                }
                return step;
            }
            const int next = clock.index + (input.delta > 0 ? 1 : -1);
            if (next < 0) {
                clock.options_tabs_focused = true;
                step.action = MenuAction::Move;
                return step;
            }
            if (next >= count) {
                return step;
            }
            step.action = MenuAction::Move;
            clock.index = next;
            return step;
        }
        if (input.horizontal != 0 && clock.options_tabs_focused) {
            clock.options_category =
                (clock.options_category + (input.horizontal > 0 ? 1 : kOptionsCategoryCount - 1)) %
                kOptionsCategoryCount;
            clock.index = 0;
            step.action = MenuAction::Move;
            return step;
        }
        const int option_row = options_category_setting_row(clock.options_category, clock.index);
        if (input.horizontal != 0 && option_row == kOptionsLanguage) {
            const int count = static_cast<int>(language_rows().size());
            if (count <= 1) {
                return step;
            }
            int next = language_row;
            if (next < 0 || next >= count) {
                next = 0;
            }
            next += input.horizontal > 0 ? 1 : -1;
            if (next < 0) {
                next = count - 1;
            } else if (next >= count) {
                next = 0;
            }
            step.action = MenuAction::Adjust;
            step.intent = MenuIntent::SetLanguage;
            step.language = next;
            step.option_row = option_row;
            step.adjust = input.horizontal > 0 ? 1 : -1;
            return step;
        }
        if (input.horizontal != 0) {
            if (!options_row_adjustable(option_row)) {
                return step;
            }
            step.action = MenuAction::Adjust;
            step.option_row = option_row;
            step.adjust = input.horizontal > 0 ? 1 : -1;
            return step;
        }
        if (input.back) {
            step.action = MenuAction::Back;
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Options, play_available, disc_loaded));
            return step;
        }
        if (!input.confirm) {
            return step;
        }
        if (clock.options_tabs_focused) {
            clock.options_tabs_focused = false;
            clock.index = 0;
            step.action = MenuAction::Move;
            return step;
        }
        step.action = MenuAction::Confirm;
        step.option_row = option_row;
        if (option_row == kOptionsBack) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Options, play_available, disc_loaded));
        } else if (option_row == kOptionsResetHighScores) {
            step.intent = MenuIntent::ResetHighScores;
        } else if (option_row == kOptionsReset) {
            step.intent = MenuIntent::ResetSettings;
        } else if (option_row == kOptionsDisc) {
            step.intent = MenuIntent::OpenDisc;
        } else if (option_row == kOptionsLanguage) {
            const int count = static_cast<int>(language_rows().size());
            int start = language_row;
            if (count <= 0 || start < 0 || start >= count) {
                start = 0;
            }
            change_page(MenuPage::Language, start);
        } else if (option_row == kOptionsTiming) {
            step.intent = MenuIntent::BeginCalibration;
        } else if (options_bind_action(option_row) >= 0) {
            step.intent = MenuIntent::BeginRebind;
        } else if (option_row == kOptionsResetControls) {
            step.intent = MenuIntent::ResetControls;
        } else if (option_row == kOptionsRibbon || option_row == kOptionsCamera ||
                   option_row == kOptionsFullscreen || option_row == kOptionsDiscAssets ||
                   option_row == kOptionsScoreDisplay || option_row == kOptionsTimingHints ||
                   option_row == kOptionsControlHints || option_row == kOptionsResolution) {
            step.intent = MenuIntent::ToggleOption;
        }
        return step;
    }

    if (input.delta != 0) {
        const int count = menu_page_count(clock.page, play_available, disc_loaded);
        if (count > 1) {
            int next = 0;
            if (clock.page == MenuPage::Wheel) {
                next = wheel_move(clock.index, input.delta > 0 ? 1 : -1);
            } else if (clock.page == MenuPage::Main || clock.page == MenuPage::Language ||
                       clock.page == MenuPage::Options || clock.page == MenuPage::Scores) {
                next = clock.index + (input.delta > 0 ? 1 : -1);
                if (next < 0 || next >= count) {
                    return step;
                }
            } else if (input.delta > 0) {
                next = (clock.index + 1) % count;
            } else {
                next = clock.index == 0 ? count - 1 : clock.index - 1;
            }
            if (next != clock.index) {
                step.action = MenuAction::Move;
                if (clips.rotate >= 0 && menu_page_uses_model(clock.page)) {
                    clock.pending_index = next;
                    begin_motion(clock, MenuMotion::Rotate);
                    return step;
                }
                clock.index = next;
            }
        }
    }

    if (input.back) {
        step.action = MenuAction::Back;
        if (clock.page == MenuPage::Main) {
            step.intent = MenuIntent::Quit;
        } else if (clock.page == MenuPage::Wheel) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Play, play_available, disc_loaded));
        } else if (clock.page == MenuPage::Music) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Music, play_available, disc_loaded));
        } else if (clock.page == MenuPage::Language) {
            clock.options_category = kOptionsCategoryInterface;
            clock.options_tabs_focused = false;
            change_page(MenuPage::Options,
                        options_category_row_count(kOptionsCategoryInterface) - 1);
        } else if (clock.page == MenuPage::Options) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Options, play_available, disc_loaded));
        } else if (clock.page == MenuPage::About) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::About, play_available, disc_loaded));
        } else if (clock.page == MenuPage::Scores) {
            change_page(MenuPage::Wheel, kWheelScores);
        }
        return step;
    }

    if (!input.confirm) {
        return step;
    }
    step.action = MenuAction::Confirm;
    switch (clock.page) {
    case MenuPage::Main:
        switch (main_menu_item(clock.index, play_available, disc_loaded)) {
        case MainItem::Play:
            change_page(MenuPage::Wheel, 0);
            break;
        case MainItem::Disc:
            step.intent = MenuIntent::OpenDisc;
            break;
        case MainItem::Music:
            if (music_ready) {
                change_page(MenuPage::Music, 0);
            } else {
                step.intent = MenuIntent::OpenMusic;
            }
            break;
        case MainItem::Options:
            clock.options_category = 0;
            clock.options_tabs_focused = true;
            change_page(MenuPage::Options, 0);
            break;
        case MainItem::About:
            change_page(MenuPage::About, 0);
            break;
        case MainItem::Quit:
            step.intent = MenuIntent::Quit;
            break;
        }
        break;
    case MenuPage::Wheel:
        if (clock.index == kWheelBack) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Play, play_available, disc_loaded));
        } else if (clock.index == kWheelScores) {
            change_page(MenuPage::Scores, 0);
        } else {
            step.intent = MenuIntent::LaunchCourse;
            step.wheel_row = clock.index;
        }
        break;
    case MenuPage::Scores:
        if (clock.index >= kDifficultyCount) {
            change_page(MenuPage::Wheel, kWheelScores);
        } else {
            // A score row is not a course. Only Back leaves.
            step.action = MenuAction::None;
        }
        break;
    case MenuPage::Music:
        if (clock.index == kMenuMusicBack) {
            change_page(MenuPage::Main,
                        main_menu_index(MainItem::Music, play_available, disc_loaded));
        } else if (clock.index == kMenuMusicChoose) {
            step.intent = MenuIntent::OpenMusic;
        } else {
            step.intent = MenuIntent::LaunchMusic;
            step.music_row = clock.index;
        }
        break;
    case MenuPage::Language:
        step.intent = MenuIntent::SetLanguage;
        step.language = clock.index;
        break;
    case MenuPage::Options:
        break;
    case MenuPage::About:
        change_page(MenuPage::Main, main_menu_index(MainItem::About, play_available, disc_loaded));
        break;
    }
    return step;
}

std::vector<Segment> menu_model_frame(const TmdModel& model,
                                      const std::vector<AnmFile>* anims,
                                      const MenuVisual& visual,
                                      ScreenRect rect,
                                      bool cull_profile_eyes) {
    std::vector<Pose> poses;
    std::span<const Pose> pose_span;
    if (anims != nullptr && visual.clip >= 0 &&
        static_cast<std::size_t>(visual.clip) < anims->size()) {
        const AnmFile& clip = (*anims)[static_cast<std::size_t>(visual.clip)];
        const int count = static_cast<int>(clip.frames.size());
        if (count > 0) {
            if (visual.loop) {
                const float rate = anm_playback_hz(clip.unk1);
                const float frames = static_cast<float>(visual.seconds) * rate;
                const int frame = static_cast<int>(std::floor(frames));
                float fraction = frames - static_cast<float>(frame);
                if (!std::isfinite(fraction)) {
                    fraction = 0.f;
                }
                poses = poses_for_frame(clip, model.objects.size(), frame, fraction, true);
            } else {
                const int last = count - 1;
                const float scaled =
                    std::clamp(visual.fraction, 0.f, 1.f) * static_cast<float>(last);
                int frame = static_cast<int>(scaled);
                if (frame > last) {
                    frame = last;
                }
                float fraction = scaled - static_cast<float>(frame);
                if (frame == last) {
                    fraction = 0.f;
                }
                poses = poses_for_frame(clip, model.objects.size(), frame, fraction, true);
            }
            pose_span = poses;
        }
    }
    if (cull_profile_eyes) {
        if (poses.empty()) {
            poses.assign(model.objects.size(), Pose{});
            for (Pose& pose : poses) {
                pose.visible = true;
            }
        }
        const TmdModel drawn =
            cull_figure_eyes(poses, model, eye_visibility_for_yaw(kEyeSideYawRadians));
        pose_span = poses;
        WireframeOptions options;
        options.packet_color = true;
        const std::vector<ModelSegment> lines = tmd_wireframe(drawn, pose_span, options);
        const std::vector<ModelSegment> viewed = to_view_space(lines, ModelView::Side);
        return fit_model_xy(viewed, rect);
    }
    WireframeOptions options;
    options.packet_color = true;
    const std::vector<ModelSegment> lines = tmd_wireframe(model, pose_span, options);
    const std::vector<ModelSegment> viewed = to_view_space(lines, ModelView::Side);
    return fit_model_xy(viewed, rect);
}

} // namespace oscilline
