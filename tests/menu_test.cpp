// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Menu clips, language rows, and which rows stay on the font.

#include "oscilline/asset/menu.hpp"
#include "oscilline/controls.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/text/glyphs.hpp"

#include <doctest/doctest.h>
#include <string>
#include <string_view>

TEST_CASE("the title character stays hidden until kShowMenuCharacter") {
    CHECK_FALSE(oscilline::kShowMenuCharacter);
}

TEST_CASE("menu clip names are high and play at the default floor") {
    using namespace oscilline;
    const auto tokens = menu_clip_tokens();
    CHECK(tokens.size() >= 6);
    for (const MenuClipToken& token : tokens) {
        CHECK(token.text[0] != '\0');
        CHECK(token.evidence_note[0] != '\0');
        CHECK(token.confidence == Confidence::High);
    }
    const std::vector<std::string> names = {"IDLE.ANM", "OPEN.ANM", "SPIN.ANM", "N01_CLOSE.ANM"};
    const MenuClipSet high = menu_clips(names, Confidence::High);
    CHECK(high.idle == 0);
    CHECK(high.open_clip == 1);
    CHECK(high.close_clip == 3);
    CHECK(high.rotate == 2);
    const MenuClipSet medium = menu_clips(names, Confidence::Medium);
    CHECK(medium.open_clip == 1);
    CHECK(medium.close_clip == 3);
    CHECK(medium.rotate == 2);
    const MenuClipSet low = menu_clips(names, Confidence::Low);
    CHECK(low.rotate == 2);
    CHECK(low.open_clip == 1);
    CHECK(low.close_clip == 3);
}

TEST_CASE("the menu clock moves, rotates, and opens") {
    using namespace oscilline;
    MenuClock clock;
    MenuClipSet clips;
    const MenuStep down = menu_step(clock, MenuInput{1, false, false}, clips, 0.f, false);
    CHECK(down.action == MenuAction::Move);
    CHECK(clock.page == MenuPage::Main);
    CHECK(menu_shown_index(clock) == kMenuMainDisc);

    clock = {};
    const MenuStep up = menu_step(clock, MenuInput{-1, false, false}, clips, 0.f, false);
    CHECK(up.action == MenuAction::None);
    CHECK(menu_shown_index(clock) == kMenuMainPlay);

    clock = {};
    const MenuStep play = menu_step(clock, MenuInput{0, true, false}, clips, 0.f, false);
    CHECK(play.action == MenuAction::Confirm);
    CHECK(play.intent == MenuIntent::None);
    CHECK(clock.page == MenuPage::Wheel);
    CHECK(clock.index == 0);
    CHECK(clock.motion == MenuMotion::Idle);

    clips.close_clip = 4;
    clips.open_clip = 5;
    clock = {};
    const MenuStep closing = menu_step(clock, MenuInput{0, true, false}, clips, 0.f, false);
    CHECK(closing.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Main);
    CHECK(clock.motion == MenuMotion::Close);
    const MenuStep opened = menu_step(clock, {}, clips, kMenuOpenSeconds, false);
    CHECK(opened.action == MenuAction::None);
    CHECK(clock.page == MenuPage::Wheel);
    CHECK(clock.motion == MenuMotion::Open);
    const MenuStep settled = menu_step(clock, {}, clips, kMenuOpenSeconds, false);
    CHECK(settled.action == MenuAction::None);
    CHECK(clock.motion == MenuMotion::Idle);
    CHECK(clock.page == MenuPage::Wheel);

    clips = {};
    clips.rotate = 2;
    clock = {};
    clock.page = MenuPage::Wheel;
    const MenuStep turning = menu_step(clock, MenuInput{1, false, false}, clips, 0.f, false);
    CHECK(turning.action == MenuAction::Move);
    CHECK(clock.motion == MenuMotion::Rotate);
    CHECK(clock.index == 0);
    CHECK(menu_shown_index(clock) == 1);
    const MenuVisual mid = menu_visual(clock, clips);
    CHECK(mid.clip == 2);
    CHECK_FALSE(mid.loop);
    const MenuStep turned =
        menu_step(clock, MenuInput{1, true, false}, clips, kMenuRotateSeconds, false);
    CHECK(turned.action == MenuAction::None);
    CHECK(clock.motion == MenuMotion::Idle);
    CHECK(clock.index == 1);
}

TEST_CASE("placeholder menus stay on labels and language picks a pak") {
    using namespace oscilline;
    CHECK(menu_row_placeholder(MenuPage::Music, 0));
    CHECK(menu_row_placeholder(MenuPage::Scores, 0));
    CHECK(menu_row_placeholder(MenuPage::Main, kMenuMainDisc));
    CHECK(menu_row_placeholder(MenuPage::Main, kMenuMainMusic));
    CHECK_FALSE(menu_row_placeholder(MenuPage::Main, kMenuMainPlay));
    CHECK_FALSE(menu_row_placeholder(MenuPage::Language, 0));
    CHECK_FALSE(menu_page_uses_model(MenuPage::Music));
    CHECK(menu_page_uses_model(MenuPage::Language));

    MenuClock clock;
    clock.index = kMenuMainDisc;
    const MenuStep disc = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(disc.intent == MenuIntent::OpenDisc);
    CHECK(clock.page == MenuPage::Main);

    clock = {};
    clock.index = kMenuMainMusic;
    const MenuStep pick = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(pick.intent == MenuIntent::OpenMusic);
    CHECK(clock.page == MenuPage::Main);
    const MenuStep ready = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, true);
    CHECK(ready.intent == MenuIntent::None);
    CHECK(clock.page == MenuPage::Music);

    clock = {};
    clock.page = MenuPage::Music;
    clock.index = 0;
    const MenuStep launch = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, true);
    CHECK(launch.intent == MenuIntent::LaunchMusic);
    CHECK(launch.music_row == 0);

    clock = {};
    clock.index = kMenuMainOptions;
    const MenuStep options = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(options.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Options);
    // Language remains available internally, but its Options row is hidden.
    for (int category = 0; category < kOptionsCategoryCount; ++category) {
        for (int row = 0; row < options_category_row_count(category); ++row) {
            CHECK(options_category_setting_row(category, row) != kOptionsLanguage);
        }
    }
    clock.page = MenuPage::Language;
    clock.index = 1;
    const MenuStep chosen = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(chosen.intent == MenuIntent::SetLanguage);
    CHECK(chosen.language == 1);
    const auto rows = language_rows();
    REQUIRE(rows.size() == 6);
    CHECK(std::string(rows[1].pak) == "GAME/02_FILES.PAK");
    CHECK(std::string(rows[0].pak) == "GAME/01_FILES.PAK");
    CHECK(std::string(rows[5].label) == "ITALIAN");
    const auto english = language_pak_for("English");
    REQUIRE(english);
    CHECK(english.value() == "GAME/02_FILES.PAK");
    CHECK_FALSE(language_pak_for("klingon"));

    clock = {};
    clock.page = MenuPage::Wheel;
    clock.index = 0;
    const MenuStep course = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(course.intent == MenuIntent::LaunchCourse);
    CHECK(course.wheel_row == 0);
    CHECK(wheel_difficulty(course.wheel_row) == Difficulty::Bronze);

    clock.index = kWheelBack;
    const MenuStep back = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(back.intent == MenuIntent::None);
    CHECK(clock.page == MenuPage::Main);

    clock = {};
    const MenuStep quit = menu_step(clock, MenuInput{0, false, true}, {}, 0.f, false);
    CHECK(quit.intent == MenuIntent::Quit);
}

TEST_CASE("title language and scores lists stop at the edge") {
    using namespace oscilline;
    MenuClock clock;
    const MenuStep edge = menu_step(clock, MenuInput{-1, false, false, 0}, {}, 0.f, false);
    CHECK(edge.action == MenuAction::None);
    CHECK(clock.index == 0);
    const MenuStep across = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(across.action == MenuAction::None);
    CHECK(clock.index == 0);

    clock.page = MenuPage::Language;
    clock.index = 0;
    const MenuStep language_edge = menu_step(clock, MenuInput{-1, false, false}, {}, 0.f, false);
    CHECK(language_edge.action == MenuAction::None);
    CHECK(clock.index == 0);
    const MenuStep language_side = menu_step(clock, MenuInput{0, false, false, -1}, {}, 0.f, false);
    CHECK(language_side.action == MenuAction::None);
    CHECK(clock.index == 0);

    clock = {};
    clock.page = MenuPage::Scores;
    clock.index = 0;
    const MenuStep side = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(side.action == MenuAction::None);
    CHECK(clock.index == 0);
    const MenuStep up = menu_step(clock, MenuInput{-1, false, false, 0}, {}, 0.f, false);
    CHECK(up.action == MenuAction::None);
    CHECK(clock.index == 0);
    const MenuStep down = menu_step(clock, MenuInput{1, false, false, 0}, {}, 0.f, false);
    CHECK(down.action == MenuAction::Move);
    CHECK(clock.page == MenuPage::Scores);
    CHECK(clock.index == 1);
    clock.index = kDifficultyCount;
    const MenuStep stuck = menu_step(clock, MenuInput{1, false, false, 0}, {}, 0.f, false);
    CHECK(stuck.action == MenuAction::None);
    CHECK(clock.index == kDifficultyCount);
    const MenuStep back = menu_step(clock, MenuInput{0, false, true, 0}, {}, 0.f, false);
    CHECK(back.action == MenuAction::Back);
    CHECK(clock.page == MenuPage::Wheel);
    CHECK(clock.index == kWheelScores);

    clock = {};
    clock.page = MenuPage::Scores;
    clock.index = kDifficultyCount;
    const MenuStep confirm = menu_step(clock, MenuInput{0, true, false, 0}, {}, 0.f, false);
    CHECK(confirm.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Wheel);
    CHECK(clock.index == kWheelScores);
}

TEST_CASE("options settings are grouped by category and horizontal values still adjust") {
    using namespace oscilline;
    MenuClock clock;
    clock.index = kMenuMainOptions;
    clock.options_category = kOptionsCategoryOther;
    clock.options_tabs_focused = false;
    const MenuStep entered = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(entered.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Options);
    CHECK(clock.index == 0);
    CHECK(clock.options_category == 0);
    CHECK(clock.options_tabs_focused);
    CHECK(menu_page_count(MenuPage::Options) == kOptionsCount);
    CHECK(options_setting_row(0) == kOptionsVideoHeader);
    CHECK(options_setting_row(options_menu_index(kOptionsMusic)) == kOptionsMusic);
    CHECK(options_setting_row(options_menu_index(kOptionsDiscAssets)) == kOptionsDiscAssets);
    CHECK(options_menu_index(kOptionsDiscAssets) < options_menu_index(kOptionsTextShake));
    for (int row = 0; row < kOptionsCount; ++row) {
        CHECK(options_setting_row(row) != kOptionsCamera);
        CHECK(options_setting_row(row) != kOptionsLanguage);
    }
    CHECK(menu_row_placeholder(MenuPage::Options, 0));
    CHECK_FALSE(menu_page_uses_model(MenuPage::Options));

    CHECK(clock.options_tabs_focused);
    const MenuStep next_category = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(next_category.action == MenuAction::Move);
    CHECK(clock.options_category == 1);
    CHECK(clock.index == 0);
    const MenuStep focus_settings = menu_step(clock, MenuInput{1, false, false}, {}, 0.f, false);
    CHECK(focus_settings.action == MenuAction::Move);
    CHECK_FALSE(clock.options_tabs_focused);

    clock.options_category = kOptionsCategoryOther;
    clock.index = 0;
    const MenuStep right = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(right.action == MenuAction::Adjust);
    CHECK(right.option_row == kOptionsRibbon);
    CHECK(right.adjust == 1);
    CHECK(clock.index == 0);

    clock.index = 2;
    const MenuStep reset_scores = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(reset_scores.intent == MenuIntent::ResetHighScores);
    CHECK(clock.page == MenuPage::Options);

    clock.index = 3;
    const MenuStep stop = menu_step(clock, MenuInput{1, false, false}, {}, 0.f, false);
    CHECK(stop.action == MenuAction::None);
    CHECK(clock.index == 3);

    clock.options_category = 1;
    clock.index = 2;
    const MenuStep timing = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(timing.intent == MenuIntent::BeginCalibration);
    CHECK(clock.page == MenuPage::Options);

    clock.options_category = 2;
    clock.index = 0;
    const MenuStep disc = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(disc.intent == MenuIntent::OpenDisc);
    CHECK(clock.page == MenuPage::Options);

    clock.options_category = kOptionsCategoryOther;
    clock.index = 1;
    const MenuStep reset = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(reset.intent == MenuIntent::ResetSettings);
    CHECK(clock.page == MenuPage::Options);

    clock.index = 3;
    const MenuStep back = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(back.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Main);
    CHECK(clock.index == kMenuMainOptions);

    clock = {};
    clock.index = kMenuMainQuit;
    const MenuStep held = menu_step(clock, MenuInput{1, false, false}, {}, 0.f, false);
    CHECK(held.action == MenuAction::None);
    CHECK(clock.index == kMenuMainQuit);

    clock = {};
    clock.page = MenuPage::Options;
    clock.options_category = 2;
    clock.options_tabs_focused = false;
    clock.index = 1;
    const MenuStep assets = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(assets.intent == MenuIntent::ToggleOption);
    CHECK(assets.option_row == kOptionsDiscAssets);
    CHECK(clock.page == MenuPage::Options);

    clock.index = 2;
    const MenuStep shake = menu_step(clock, MenuInput{0, false, false, -1}, {}, 0.f, false);
    CHECK(shake.action == MenuAction::Adjust);
    CHECK(shake.option_row == kOptionsTextShake);
    CHECK(shake.adjust == -1);
    CHECK(shake.intent == MenuIntent::None);
    CHECK(clock.page == MenuPage::Options);

    CHECK(options_category_setting_row(2, 6) == kOptionsBack);

    clock.index = 3;
    const MenuStep score = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(score.action == MenuAction::Adjust);
    CHECK(score.option_row == kOptionsScoreDisplay);
    clock.index = 4;
    const MenuStep timing_hints = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(timing_hints.intent == MenuIntent::ToggleOption);
    CHECK(timing_hints.option_row == kOptionsTimingHints);
    clock.index = 5;
    const MenuStep hints = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(hints.intent == MenuIntent::ToggleOption);
    CHECK(hints.option_row == kOptionsControlHints);

    clock.options_category = kOptionsCategoryControls;
    clock.index = 2;
    const MenuStep rebind = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(rebind.intent == MenuIntent::BeginRebind);
    CHECK(rebind.option_row == kOptionsBindWave);
    CHECK(options_bind_action(rebind.option_row) == static_cast<int>(GameAction::Wave));
    const MenuStep sideways = menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false);
    CHECK(sideways.action == MenuAction::None);
    clock.index = 4;
    const MenuStep reset_controls = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false);
    CHECK(reset_controls.intent == MenuIntent::ResetControls);
    CHECK(clock.page == MenuPage::Options);
}

TEST_CASE("play original is hidden until a disc course can start") {
    using namespace oscilline;
    CHECK(main_menu_count(true) == kMenuMainCount);
    CHECK(main_menu_count(false) == 5);
    CHECK(menu_page_count(MenuPage::Main, false) == 5);
    CHECK(main_menu_count(true, true) == kMenuMainCount - 1);
    CHECK(main_menu_count(false, true) == 4);
    CHECK(menu_page_count(MenuPage::Main, false, true) == 4);
    CHECK(main_menu_item(0, true, true) == MainItem::Play);
    CHECK(main_menu_item(1, true, true) == MainItem::Music);
    CHECK(main_menu_index(MainItem::Disc, true, true) == 0);
    CHECK(std::string(main_menu_label(MainItem::Play)) == "PLAY ORIGINAL");
    CHECK(std::string(main_menu_label(MainItem::Disc)) == "LOAD DISC");
    CHECK(std::string(main_menu_label(MainItem::Music)) == "PLAY MY MUSIC");
    CHECK(main_menu_item(0, false) == MainItem::Disc);
    CHECK(main_menu_index(MainItem::Play, false) == 0);
    CHECK(main_menu_index(MainItem::Music, false) == 1);
    CHECK(main_menu_index(MainItem::Options, false) == 2);
    CHECK(main_menu_index(MainItem::About, false) == 3);
    CHECK(std::string(main_menu_label(MainItem::About)) == "ABOUT");

    MenuClock about;
    about.index = main_menu_index(MainItem::About, false);
    const MenuStep open_about = menu_step(about, MenuInput{0, true, false}, {}, 0.f, false, false);
    CHECK(open_about.action == MenuAction::Confirm);
    CHECK(about.page == MenuPage::About);
    CHECK(menu_page_count(MenuPage::About, false) == 1);
    CHECK_FALSE(menu_page_uses_model(MenuPage::About));
    const MenuStep close_about = menu_step(about, MenuInput{0, false, true}, {}, 0.f, false, false);
    CHECK(close_about.action == MenuAction::Back);
    CHECK(about.page == MenuPage::Main);
    CHECK(about.index == main_menu_index(MainItem::About, false));

    about.page = MenuPage::About;
    about.index = 0;
    const MenuStep confirm_back =
        menu_step(about, MenuInput{0, true, false}, {}, 0.f, false, false);
    CHECK(confirm_back.action == MenuAction::Confirm);
    CHECK(about.page == MenuPage::Main);
    CHECK(about.index == main_menu_index(MainItem::About, false));

    MenuClock clock;
    const MenuStep hidden = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false, false);
    CHECK(hidden.intent == MenuIntent::OpenDisc);
    CHECK(clock.page == MenuPage::Main);

    clock = {};
    clock.page = MenuPage::Wheel;
    clock.index = kWheelBack;
    const MenuStep wheel_back = menu_step(clock, MenuInput{0, true, false}, {}, 0.f, false, false);
    CHECK(wheel_back.action == MenuAction::Confirm);
    CHECK(clock.page == MenuPage::Main);
    CHECK(clock.index == 0);
    CHECK(main_menu_item(clock.index, false) == MainItem::Disc);

    MenuClock loaded_disc_clock;
    const MenuStep loaded_disc_first_row =
        menu_step(loaded_disc_clock, MenuInput{0, true, false}, {}, 0.f, false, false, 1, true);
    CHECK(loaded_disc_first_row.intent == MenuIntent::OpenMusic);
}

TEST_CASE("option categories group their settings for the horizontal category strip") {
    using namespace oscilline;
    CHECK(options_category_name(kOptionsCategoryVideo) == std::string_view("VIDEO"));
    CHECK(options_category_name(kOptionsCategoryAudio) == std::string_view("AUDIO"));
    CHECK(options_category_name(kOptionsCategoryInterface) == std::string_view("INTERFACE"));
    CHECK(options_category_name(kOptionsCategoryControls) == std::string_view("CONTROLS"));
    CHECK(options_category_name(kOptionsCategoryOther) == std::string_view("OTHER"));
    CHECK(options_category_row_count(kOptionsCategoryVideo) == 5);
    CHECK(options_category_row_count(kOptionsCategoryAudio) == 4);
    CHECK(options_category_row_count(kOptionsCategoryInterface) == 7);
    CHECK(options_category_row_count(kOptionsCategoryControls) == 6);
    CHECK(options_category_row_count(kOptionsCategoryOther) == 4);
    CHECK(options_category_setting_row(0, 0) == kOptionsAspect);
    CHECK(options_category_setting_row(1, 2) == kOptionsTiming);
    CHECK(options_category_setting_row(2, 1) == kOptionsDiscAssets);
    CHECK(options_category_setting_row(2, 2) == kOptionsTextShake);
    CHECK(options_category_setting_row(2, 3) == kOptionsScoreDisplay);
    CHECK(options_category_setting_row(2, 4) == kOptionsTimingHints);
    CHECK(options_category_setting_row(2, 5) == kOptionsControlHints);
    CHECK(options_category_setting_row(2, 6) == kOptionsBack);
    CHECK(options_category_setting_row(0, 3) == kOptionsResolution);
    CHECK(options_category_setting_row(0, 4) == kOptionsBack);
    CHECK(options_category_setting_row(1, 3) == kOptionsBack);
    CHECK(options_category_setting_row(0, 5) == -1);
    CHECK(options_category_setting_row(2, 7) == -1);
    for (int action = 0; action < kGameActionCount; ++action) {
        CHECK(options_category_setting_row(kOptionsCategoryControls, action) ==
              kOptionsBindBlock + action);
    }
    CHECK(options_category_setting_row(3, 4) == kOptionsResetControls);
    CHECK(options_category_setting_row(3, 5) == kOptionsBack);
    CHECK(options_category_setting_row(4, 0) == kOptionsRibbon);
    CHECK(options_category_setting_row(4, 1) == kOptionsReset);
    CHECK(options_category_setting_row(4, 2) == kOptionsResetHighScores);
    CHECK(options_category_setting_row(4, 3) == kOptionsBack);
    CHECK(options_row_adjustable(kOptionsScoreDisplay));
    CHECK(options_row_adjustable(kOptionsTimingHints));
    CHECK(options_row_adjustable(kOptionsResolution));
    CHECK(options_row_adjustable(kOptionsControlHints));
    CHECK_FALSE(options_row_adjustable(kOptionsBindBlock));
    CHECK_FALSE(options_row_adjustable(kOptionsResetControls));
    CHECK_FALSE(options_row_adjustable(kOptionsBack));
    CHECK(options_bind_action(kOptionsResetControls) == -1);
}

TEST_CASE("slash has no disc-font object index") {
    CHECK_FALSE(oscilline::glyph_object(U'/').has_value());
}

TEST_CASE("options category strip navigates separately from setting values") {
    using namespace oscilline;
    MenuClock clock;
    clock.page = MenuPage::Options;
    const MenuStep focus_tabs =
        menu_step(clock, MenuInput{-1, false, false}, {}, 0.f, false, false);
    CHECK(focus_tabs.action == MenuAction::Move);
    CHECK(clock.options_tabs_focused);

    const MenuStep next_category =
        menu_step(clock, MenuInput{0, false, false, 1}, {}, 0.f, false, false);
    CHECK(next_category.action == MenuAction::Move);
    CHECK(clock.options_category == 1);
    CHECK(clock.index == 0);

    const MenuStep focus_settings =
        menu_step(clock, MenuInput{1, false, false}, {}, 0.f, false, false);
    CHECK(focus_settings.action == MenuAction::Move);
    CHECK_FALSE(clock.options_tabs_focused);
    CHECK(clock.index == 0);

    for (int category = 0; category < kOptionsCategoryCount; ++category) {
        MenuClock back;
        back.page = MenuPage::Options;
        back.options_category = category;
        back.index = options_category_row_count(category) - 1;
        const MenuStep selected = menu_step(back, MenuInput{0, true, false}, {}, 0.f, false, false);
        CHECK(selected.action == MenuAction::Confirm);
        CHECK(back.page == MenuPage::Main);
        CHECK(back.index == main_menu_index(MainItem::Options, false));
    }
}
