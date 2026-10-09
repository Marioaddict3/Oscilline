// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Title, language list, difficulty wheel, and the music picker.

#include "title.hpp"

#include "oscilline/asset/menu.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/audio/menu_sfx.hpp"
#include "oscilline/course/difficulty.hpp"
#include "oscilline/course/music.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/present/host.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/settings.hpp"
#include "session.hpp"
#include "sfx_stage.hpp"
#include "text_out.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

constexpr const char* kMusicLabel[kMenuMusicCount] = {
    "BRONZE", "SILVER", "GOLD", "CHOOSE FILE", "BACK"};
constexpr const char* kWheelLabel[kWheelSlots] = {"BRONZE", "SILVER", "GOLD", "SCORES", "BACK"};

// The frame is 36 px in. Footer text is padded so an 8 px debug glyph and a
// 12 px disc glyph both sit inside that line.
constexpr float kMenuFrame = 36.f;
constexpr float kFooterPad = 12.f;
constexpr float kFooterText = 12.f;
constexpr float kFooterX = kMenuFrame + kFooterPad;
constexpr float kFooterBottom =
    static_cast<float>(Host::kHeight) - kMenuFrame - kFooterPad - kFooterText;

void add(std::vector<Segment>& out, float x0, float y0, float x1, float y1) {
    Segment segment;
    segment.x0 = x0;
    segment.y0 = y0;
    segment.x1 = x1;
    segment.y1 = y1;
    out.push_back(segment);
}

// A filled disc as wide as the default stroke core plus half its feather.
void add_join(std::vector<FilledTriangle>& out, float x, float y, Rgb color, float depth) {
    constexpr int kSides = 12;
    constexpr float kRadius = 0.5f * StrokeStyle{}.width + 0.5f * StrokeStyle{}.feather;
    constexpr float kStep = 6.28318531f / static_cast<float>(kSides);
    for (int i = 0; i < kSides; ++i) {
        const float a0 = kStep * static_cast<float>(i);
        const float a1 = kStep * static_cast<float>(i + 1);
        FilledTriangle triangle;
        triangle.x0 = x;
        triangle.y0 = y;
        triangle.x1 = x + kRadius * std::cos(a0);
        triangle.y1 = y + kRadius * std::sin(a0);
        triangle.x2 = x + kRadius * std::cos(a1);
        triangle.y2 = y + kRadius * std::sin(a1);
        triangle.depth = depth;
        triangle.color = color;
        out.push_back(triangle);
    }
}

TriangleList title_mark() {
    std::vector<Segment> segments;
    const float offset =
        (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    const float left = kMenuFrame + offset;
    const float right = static_cast<float>(kLogicalWidth) - kMenuFrame + offset;
    constexpr float kBottom = static_cast<float>(Host::kHeight) - kMenuFrame;
    add(segments, left, kMenuFrame, right, kMenuFrame);
    add(segments, right, kMenuFrame, right, kBottom);
    add(segments, right, kBottom, left, kBottom);
    add(segments, left, kBottom, left, kMenuFrame);
    // The app icon mark (packaging/icons/make_icons.py): a white diamond with
    // an orange ribbon through it. Icon units are scaled so the diamond's
    // 72-unit half-width is 40 px.
    const float cx = static_cast<float>(kLogicalWidth) * 0.5f + offset;
    constexpr float kCy = 112.f;
    constexpr float kScale = 40.f / 72.f;
    constexpr float kDiamond[][2] = {{0.f, -72.f}, {72.f, 0.f}, {0.f, 72.f}, {-72.f, 0.f}};
    for (std::size_t i = 0; i < std::size(kDiamond); ++i) {
        const auto& from = kDiamond[i];
        const auto& to = kDiamond[(i + 1) % std::size(kDiamond)];
        add(segments,
            cx + from[0] * kScale,
            kCy + from[1] * kScale,
            cx + to[0] * kScale,
            kCy + to[1] * kScale);
        // Farther than the ribbon, so the ribbon is drawn over it as on the icon.
        segments.back().depth = 1.f;
    }
    constexpr Rgb kRibbonColor{245.f / 255.f, 142.f / 255.f, 39.f / 255.f};
    constexpr float kRibbon[][2] = {{-84.f, 0.f},
                                    {-42.f, 0.f},
                                    {-28.f, -24.f},
                                    {-14.f, 24.f},
                                    {0.f, -24.f},
                                    {14.f, 24.f},
                                    {28.f, -24.f},
                                    {42.f, 0.f},
                                    {84.f, 0.f}};
    for (std::size_t i = 0; i + 1 < std::size(kRibbon); ++i) {
        add(segments,
            cx + kRibbon[i][0] * kScale,
            kCy + kRibbon[i][1] * kScale,
            cx + kRibbon[i + 1][0] * kScale,
            kCy + kRibbon[i + 1][1] * kScale);
        segments.back().color = kRibbonColor;
    }
    // Separate segments leave a notch on the outside of each sharp corner.
    // A small disc at every interior ribbon point and diamond corner rounds
    // those joins.
    std::vector<FilledTriangle> joins;
    for (std::size_t i = 0; i < std::size(kDiamond); ++i) {
        add_join(joins, cx + kDiamond[i][0] * kScale, kCy + kDiamond[i][1] * kScale, Rgb{}, 1.f);
    }
    for (std::size_t i = 1; i + 1 < std::size(kRibbon); ++i) {
        add_join(
            joins, cx + kRibbon[i][0] * kScale, kCy + kRibbon[i][1] * kScale, kRibbonColor, 0.f);
    }
    TriangleList triangles;
    append_strokes(triangles, segments, {}, joins);
    return triangles;
}

void glyph(TriangleList& triangles,
           std::vector<TextGlyph>& text,
           const TextPainter& painter,
           float x,
           float y,
           std::string value) {
    x += (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    TextTarget target;
    target.glyphs = &text;
    target.triangles = &triangles;
    painter.line(target, x, y, std::move(value));
}

void centered_glyph(TriangleList& triangles,
                    std::vector<TextGlyph>& text,
                    const TextPainter& painter,
                    float y,
                    std::string value) {
    const float x = (static_cast<float>(logical_width()) - painter.measure_width(value)) * 0.5f;
    TextTarget target;
    target.glyphs = &text;
    target.triangles = &triangles;
    painter.line(target, x, y, std::move(value));
}

std::string on_off(bool value) {
    return value ? "ON" : "OFF";
}

std::string offset_text(int offset_ms) {
    if (offset_ms > 0) {
        return "+" + std::to_string(offset_ms) + " MS";
    }
    return std::to_string(offset_ms) + " MS";
}

std::string scale_text(int scale) {
    if (scale <= 0) {
        return "DEFAULT";
    }
    return std::to_string(scale) + "X";
}

std::string text_shake_text(int tenths) {
    if (tenths < kTextShakeMin) {
        tenths = kTextShakeMin;
    }
    if (tenths > kTextShakeMax) {
        tenths = kTextShakeMax;
    }
    if (tenths <= 0) {
        return "OFF";
    }
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "X";
}

enum class DiscPathStatus { None, NotFound, Loaded };

DiscPathStatus disc_path_status(const std::filesystem::path& disc) {
    if (disc.empty()) {
        return DiscPathStatus::None;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(disc, error) || error) {
        return DiscPathStatus::NotFound;
    }
    return DiscPathStatus::Loaded;
}

const char* disc_status_text(DiscPathStatus status) {
    switch (status) {
    case DiscPathStatus::Loaded:
        return "LOADED";
    case DiscPathStatus::NotFound:
        return "NOT FOUND";
    case DiscPathStatus::None:
        return "NONE";
    }
    return "NONE";
}

std::string disc_row_text(const std::filesystem::path& disc, std::string_view region) {
    const DiscPathStatus status = disc_path_status(disc);
    if (status == DiscPathStatus::None) {
        return "NONE";
    }
    if (status == DiscPathStatus::NotFound) {
        return disc_status_text(status);
    }
    return region.empty() ? "UNKNOWN" : std::string(region);
}

const char* language_label(std::string_view pak) {
    for (const LanguageRow& row : language_rows()) {
        if (pak == row.pak) {
            return row.label;
        }
    }
    return "ENGLISH";
}

int language_row_for(std::string_view pak) {
    const auto rows = language_rows();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (pak == rows[static_cast<std::size_t>(i)].pak) {
            return i;
        }
    }
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (std::string_view(rows[static_cast<std::size_t>(i)].label) == "ENGLISH") {
            return i;
        }
    }
    return 0;
}

const char* aspect_ratio_label(ViewportAspect aspect) {
    switch (aspect) {
    case ViewportAspect::FourThree:
        return "4:3";
    case ViewportAspect::SixteenNine:
        return "16:9";
    case ViewportAspect::Unrestricted:
        return "UNRESTRICTED";
    }
    return "4:3";
}

ViewportAspect next_aspect_ratio(ViewportAspect aspect, int delta) {
    constexpr int kAspectCount = 3;
    const int step = delta > 0 ? 1 : -1;
    const int index = (static_cast<int>(aspect) + step + kAspectCount) % kAspectCount;
    return static_cast<ViewportAspect>(index);
}

std::string option_value(int row,
                         const Settings& settings,
                         const std::filesystem::path& disc,
                         bool assets,
                         std::string_view language,
                         std::string_view disc_region) {
    switch (row) {
    case kOptionsRibbon:
        return "RIBBON GUIDES:  " + on_off(settings.ribbon_guides);
    case kOptionsMusic:
        return "MUSIC:  " + std::to_string(settings.playback.music_volume) + "%";
    case kOptionsSfx:
        return "SFX:  " + std::to_string(settings.playback.sfx_volume) + "%";
    case kOptionsTiming:
        return "AUDIO OFFSET:  " + offset_text(settings.playback.timing_offset_ms);
    case kOptionsCamera:
        return std::string("CAMERA:  ") + (settings.disc_camera ? "DISC" : "BUILT-IN");
    case kOptionsDisc:
        return "DISC:  " + disc_row_text(disc, disc_region);
    case kOptionsFullscreen:
        return "FULLSCREEN:  " + on_off(settings.fullscreen);
    case kOptionsWindow:
        return "WINDOW SCALE:  " + scale_text(settings.window_scale);
    case kOptionsResolution:
        return std::string("RESOLUTION:  ") + (settings.classic_resolution ? "CLASSIC" : "HD");
    case kOptionsTextShake:
        return "TEXT SHAKE:  " + text_shake_text(settings.text_shake);
    case kOptionsDiscAssets:
        return "DISC ASSETS:  " + on_off(assets);
    case kOptionsLanguage:
        return std::string("LANGUAGE:  ") + language_label(language);
    case kOptionsAspect:
        return std::string("ASPECT RATIO:  ") + aspect_ratio_label(settings.aspect_ratio);
    case kOptionsVideoHeader:
        return "VIDEO";
    case kOptionsAudioHeader:
        return "AUDIO";
    case kOptionsInterfaceHeader:
        return "INTERFACE";
    case kOptionsOtherHeader:
        return "OTHER";
    case kOptionsScoreDisplay:
        return std::string("SCORE:  ") + (settings.hud.score_coupons ? "COUPONS" : "NUMBER");
    case kOptionsTimingHints:
        return "TIMING HINTS:  " + std::string(settings.hud.timing_hints ? "SHOW" : "HIDE");
    case kOptionsControlHints:
        return "CONTROL HINTS:  " + std::string(settings.hud.control_hints ? "SHOW" : "HIDE");
    case kOptionsBindBlock:
    case kOptionsBindLoop:
    case kOptionsBindWave:
    case kOptionsBindPit: {
        const int action = options_bind_action(row);
        const auto index = static_cast<std::size_t>(action);
        return std::string(game_action_label(static_cast<GameAction>(action))) + ":  " +
               key_label(settings.controls.keys[index]) + " / " +
               pad_button_label(settings.controls.pad[index]);
    }
    case kOptionsControlsHeader:
        return "CONTROLS";
    case kOptionsResetControls:
        return "RESET CONTROLS";
    case kOptionsReset:
        return "RESET SETTINGS";
    case kOptionsResetHighScores:
        return "RESET HIGH SCORES";
    default:
        return "BACK";
    }
}

std::vector<std::int16_t> calibration_click() {
    constexpr int kRate = 44100;
    constexpr int kCount = kRate / 50;
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kCount);
        const float env = (1.f - u) * (1.f - u);
        const float phase = static_cast<float>(i) * 6.2831853f * 1000.f / static_cast<float>(kRate);
        pcm[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(std::sin(phase) * env * 20000.f);
    }
    return pcm;
}

void flash_marker(TriangleList& picture) {
    std::vector<Segment> segments;
    const float kX =
        400.f + (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    constexpr float kY = 96.f;
    const auto add = [&](float x0, float y0, float x1, float y1) {
        Segment segment;
        segment.x0 = x0;
        segment.y0 = y0;
        segment.x1 = x1;
        segment.y1 = y1;
        segments.push_back(segment);
    };
    add(kX, kY - 28.f, kX + 18.f, kY);
    add(kX + 18.f, kY, kX, kY + 28.f);
    add(kX, kY + 28.f, kX - 18.f, kY);
    add(kX - 18.f, kY, kX, kY - 28.f);
    append_strokes(picture, segments);
}

std::string fit(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

Slot page_model_slot(MenuPage page) {
    if (page == MenuPage::Language) {
        return Slot::MenuLabels;
    }
    return Slot::MenuWheel;
}

void paint_slot(TriangleList& picture,
                AssetRegistry& assets,
                Slot slot,
                const MenuVisual& visual,
                ScreenRect rect,
                bool cull_profile_eyes = false) {
    // The only switch for the title character. Wheel and label slots still draw.
    if (slot == Slot::MenuModel && !kShowMenuCharacter) {
        return;
    }
    if (assets.origin(slot) != AssetOrigin::Disc) {
        return;
    }
    rect.left += (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    rect.right += (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    const TmdModel* model = assets.model(slot);
    if (model == nullptr) {
        return;
    }
    const std::vector<Segment> fitted =
        menu_model_frame(*model, assets.animations(slot), visual, rect, cull_profile_eyes);
    append_strokes(picture, fitted);
}

constexpr std::uint32_t kKoFiQrRows[29] = {
    0x1fc5897fu, 0x10577e41u, 0x175bc55du, 0x174e5c5du, 0x1746f05du, 0x10408b41u,
    0x1fd5557fu, 0x000dda00u, 0x14886cc5u, 0x18d877adu, 0x16aa88d2u, 0x02a0238au,
    0x10f9aceau, 0x18d90994u, 0x110b6dc7u, 0x01d1d9b7u, 0x10d06552u, 0x1c5a7910u,
    0x126e906fu, 0x01fc372cu, 0x0bf1a157u, 0x17110f00u, 0x1155717fu, 0x1115c441u,
    0x03f9645du, 0x0f2c485du, 0x1922d55du, 0x03f0f441u, 0x12948b7fu,
};

void add_filled_rect(std::vector<FilledTriangle>& fills,
                     float left,
                     float top,
                     float right,
                     float bottom,
                     Rgb color) {
    FilledTriangle first;
    first.x0 = left;
    first.y0 = top;
    first.x1 = right;
    first.y1 = top;
    first.x2 = right;
    first.y2 = bottom;
    first.color = color;
    fills.push_back(first);

    FilledTriangle second;
    second.x0 = left;
    second.y0 = top;
    second.x1 = right;
    second.y1 = bottom;
    second.x2 = left;
    second.y2 = bottom;
    second.color = color;
    fills.push_back(second);
}

void paint_kofi_qr(TriangleList& picture, float top) {
    constexpr float kModule = 4.f;
    constexpr int kQrModules = 33;
    constexpr int kQuietModules = 2;
    constexpr float kSize = static_cast<float>(kQrModules) * kModule;
    const float left = (static_cast<float>(logical_width()) - kSize) * 0.5f;

    const std::vector<Segment> no_strokes;
    std::vector<FilledTriangle> background;
    add_filled_rect(background, left, top, left + kSize, top + kSize, Rgb{1.f, 1.f, 1.f, 1.f});
    append_strokes(picture, no_strokes, {}, background);

    std::vector<FilledTriangle> modules;
    for (int row = 0; row < 29; ++row) {
        for (int column = 0; column < 29; ++column) {
            if ((kKoFiQrRows[row] & (1u << column)) == 0) {
                continue;
            }
            const float x = left + static_cast<float>(column + kQuietModules) * kModule;
            const float y = top + static_cast<float>(row + kQuietModules) * kModule;
            add_filled_rect(modules, x, y, x + kModule, y + kModule, Rgb{0.f, 0.f, 0.f, 1.f});
        }
    }
    append_strokes(picture, no_strokes, {}, modules);
}

void menu_row(TriangleList& picture,
              std::vector<TextGlyph>& text,
              const TextPainter& painter,
              float x,
              float y,
              bool selected,
              std::string label) {
    glyph(picture, text, painter, x, y, "    " + std::move(label));
    if (selected) {
        const float menu_offset =
            (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
        paint_menu_selector(picture, x + menu_offset - 14.f, y);
    }
}

enum class OptionChange { None, Changed, Locked };

// Applies left or right (`delta` -1 or +1) or confirm (`delta` 0) to an Options
// row. Every value list loops: past the last value is the first, and back.
// Confirm flips a switch and steps a cycle forward; it leaves number rows
// alone. Changes go to both `effective` and `stored`. The caller saves.
OptionChange change_option(int row,
                           int delta,
                           Settings& effective,
                           Settings& stored,
                           Host& host,
                           bool disc_assets_locked) {
    // Every value list loops. A switch has two values, so left, right, and
    // confirm all flip it.
    const auto set_switch = [&](bool& live, bool& saved) {
        live = !live;
        saved = live;
        return OptionChange::Changed;
    };
    // Numbers step by `delta` and wrap from one end to the other.
    const auto step_number = [&](int& live, int& saved, int lo, int hi) {
        if (delta == 0) {
            return OptionChange::None;
        }
        int next = live + delta;
        if (next > hi) {
            next = lo;
        } else if (next < lo) {
            next = hi;
        }
        if (next == live) {
            return OptionChange::None;
        }
        live = next;
        saved = next;
        return OptionChange::Changed;
    };
    switch (row) {
    case kOptionsRibbon:
        return set_switch(effective.ribbon_guides, stored.ribbon_guides);
    case kOptionsCamera:
        return set_switch(effective.disc_camera, stored.disc_camera);
    case kOptionsTimingHints:
        return set_switch(effective.hud.timing_hints, stored.hud.timing_hints);
    case kOptionsResolution: {
        const OptionChange change =
            set_switch(effective.classic_resolution, stored.classic_resolution);
        if (change == OptionChange::Changed) {
            host.set_classic_resolution(effective.classic_resolution);
        }
        return change;
    }
    case kOptionsControlHints:
        return set_switch(effective.hud.control_hints, stored.hud.control_hints);
    case kOptionsScoreDisplay:
        return set_switch(effective.hud.score_coupons, stored.hud.score_coupons);
    case kOptionsFullscreen: {
        const OptionChange change = set_switch(effective.fullscreen, stored.fullscreen);
        if (change == OptionChange::Changed) {
            host.set_fullscreen(effective.fullscreen);
        }
        return change;
    }
    case kOptionsDiscAssets:
        if (disc_assets_locked) {
            return OptionChange::Locked;
        }
        return set_switch(effective.disc_assets, stored.disc_assets);
    case kOptionsMusic:
        return step_number(
            effective.playback.music_volume, stored.playback.music_volume, 0, kVolumePercentMax);
    case kOptionsSfx:
        return step_number(
            effective.playback.sfx_volume, stored.playback.sfx_volume, 0, kVolumePercentMax);
    case kOptionsTiming:
        return step_number(effective.playback.timing_offset_ms,
                           stored.playback.timing_offset_ms,
                           kTimingOffsetMinMs,
                           kTimingOffsetMaxMs);
    case kOptionsTextShake:
        return step_number(effective.text_shake, stored.text_shake, kTextShakeMin, kTextShakeMax);
    case kOptionsWindow: {
        // Steps past either end wrap around, the same as the aspect ratio.
        const int max_scale = host.max_window_scale();
        int next = effective.window_scale + (delta == 0 ? 1 : delta);
        if (next < 0) {
            next = max_scale;
        } else if (next > max_scale) {
            next = 0;
        }
        if (next == effective.window_scale) {
            return OptionChange::None;
        }
        effective.window_scale = host.set_window_scale(next);
        stored.window_scale = effective.window_scale;
        return OptionChange::Changed;
    }
    case kOptionsAspect:
        effective.aspect_ratio = next_aspect_ratio(effective.aspect_ratio, delta == 0 ? 1 : delta);
        stored.aspect_ratio = effective.aspect_ratio;
        host.set_aspect_ratio(effective.aspect_ratio);
        effective.window_scale = host.set_window_scale(effective.window_scale);
        stored.window_scale = effective.window_scale;
        return OptionChange::Changed;
    default:
        return OptionChange::None;
    }
}

// The title screen and its menus. One frame: tick the notice, collect picked
// files, update the active mode (timing test, rebinding, or menu input), apply
// the step, draw, then load picked music or launch a course.
class TitleScreen {
  public:
    TitleScreen(Host& host,
                TitleStart start,
                Settings effective,
                Settings stored,
                std::string_view language_pak,
                const RunFlags& flags,
                bool disc_assets_locked);

    // Returns 0 when the window closes or the user quits.
    int run();

  private:
    // Values one frame's update, draw, and launch share.
    struct Frame {
        int steps = 0;
        // The page when the frame began. Menu cues follow the page left.
        MenuPage page = MenuPage::Main;
        Slot model_slot{};
        MenuClipSet clips;
        MenuStep menu;
        bool flash = false;
        bool refresh_presentation = false;
    };

    struct Canvas {
        TriangleList& picture;
        std::vector<TextGlyph>& text;
        const TextPainter& painter;
        int shown = 0;
    };

    // The timing test on Options > Audio.
    struct Calibration {
        bool active = false;
        std::int64_t elapsed_ms = 0;
        std::int64_t next_click_ms = kCalibrationIntervalMs;
        std::int64_t last_click_ms = -100000;
        std::vector<int> samples;
        int previous_offset = 0;
    };

    [[nodiscard]] bool disc_loaded() const { return disc_region_ == "PAL" || disc_region_ == "JP"; }
    [[nodiscard]] bool play_ok() const { return disc_mounted_ && disc_assets_; }

    void save_stored();
    void enter_play();
    void leave_play();
    void reload_presentation();
    void sync_menu_audio();
    // 1 cached, 0 failed, -1 the window closed.
    int load_cached(const std::filesystem::path& file);
    void show_music_row();

    void tick_notice();
    void take_picked_files();
    void update_calibration(const HostInput& input, Frame& frame);
    void update_rebind(const HostInput& input);
    void update_menu(const HostInput& input, Frame& frame);
    void apply_settings_intent(Frame& frame);
    void apply_intent(Frame& frame);

    void draw(const Frame& frame);
    void draw_models(Canvas& canvas, const Frame& frame);
    void draw_main(Canvas& canvas);
    void draw_wheel(Canvas& canvas);
    void draw_music(Canvas& canvas);
    void draw_language(Canvas& canvas);
    void draw_options(Canvas& canvas, const Frame& frame);
    void draw_about(Canvas& canvas);
    void draw_scores(Canvas& canvas);
    void draw_footer(Canvas& canvas);

    // False when the window closed while decoding.
    bool open_pending_music();
    void launch_course(int wheel_row);
    void launch_music(int row);

    Host& host_;
    Settings effective_;
    // The file copy, without command-line overrides.
    Settings stored_;
    RunFlags flags_;
    bool disc_assets_locked_ = false;
    std::filesystem::path disc_;
    std::filesystem::path music_;
    std::string language_;
    // Mirrors effective_.disc_assets; Options changes both.
    bool disc_assets_ = true;
    SfxStage sfx_;
    std::vector<std::int16_t> click_;

    MenuClock clock_;
    ScoreBoard scores_;
    MusicScoreBook music_scores_;
    std::optional<DecodedMusic> cached_;
    std::filesystem::path cached_path_;
    std::filesystem::path pending_;
    std::string music_key_;
    std::string notice_;
    std::string timed_notice_;
    std::int64_t notice_since_ms_ = 0;
    std::int64_t ui_ms_ = 0;
    std::optional<std::pair<bool, bool>> main_rows_was_;
    Calibration calib_;
    // GameAction waiting for its next key or pad button, or -1.
    int rebind_action_ = -1;

    DebugTextPainter debug_text_;
    std::optional<DiscTextPainter> disc_text_;
    AssetRegistry menu_assets_ = AssetRegistry::placeholders();
    bool disc_mounted_ = false;
    std::string disc_region_ = "NONE";
    // What reload_presentation and sync_menu_audio last applied.
    std::optional<std::filesystem::path> shown_disc_;
    std::string shown_language_;
    bool shown_assets_ = false;
    bool audio_on_ = false;
    std::filesystem::path audio_disc_;
};

TitleScreen::TitleScreen(Host& host,
                         TitleStart start,
                         Settings effective,
                         Settings stored,
                         std::string_view language_pak,
                         const RunFlags& flags,
                         bool disc_assets_locked)
    : host_(host), effective_(std::move(effective)), stored_(std::move(stored)), flags_(flags),
      disc_assets_locked_(disc_assets_locked), disc_(std::move(start.disc)),
      music_(std::move(start.music)), language_(language_pak), disc_assets_(effective_.disc_assets),
      sfx_(flags.sfx_experimental), click_(calibration_click()) {
    stored_.disc_path = disc_.string();
    if (!disc_.empty()) {
        save_stored();
    }
    if (start.open_wheel >= 0 && start.open_wheel < kDifficultyCount && music_.empty()) {
        clock_.page = MenuPage::Wheel;
        clock_.index = start.open_wheel;
    }
    if (start.open_music >= 0 && start.open_music < kDifficultyCount && !music_.empty()) {
        clock_.page = MenuPage::Music;
        clock_.index = start.open_music;
    }
    if (auto loaded = load_high_scores(high_score_path())) {
        scores_ = loaded.value();
    } else {
        std::cerr << "oscilline: " << loaded.error() << '\n';
    }
    notice_ = disc_.empty() && music_.empty() ? "NO DISC" : "";
    sfx_.set_playback(playback_from(effective_));
    if (effective_.window_scale > 0) {
        effective_.window_scale = host_.set_window_scale(effective_.window_scale);
    }
    if (effective_.fullscreen) {
        host_.set_fullscreen(true);
    }
}

void TitleScreen::save_stored() {
    if (auto saved = save_settings(settings_path(), stored_); !saved) {
        std::cerr << "oscilline: " << saved.error() << '\n';
    }
}

// Stop is queued. Drain applies it before the device pauses, or the next
// mix silences the loop leave_play thought was still running.
void TitleScreen::enter_play() {
    sfx_.mixer().stop(SfxId::MenuLoop);
    sfx_.mixer().drain();
    sfx_.discard();
    sfx_.set_paused(true);
}

void TitleScreen::leave_play() {
    if (ensure_menu_loop(sfx_.mixer())) {
        sfx_.discard();
    }
    sfx_.set_paused(false);
}

void TitleScreen::reload_presentation() {
    if (shown_disc_ && *shown_disc_ == disc_ && shown_language_ == language_ &&
        shown_assets_ == disc_assets_) {
        return;
    }
    shown_disc_ = disc_;
    shown_language_ = language_;
    shown_assets_ = disc_assets_;
    disc_text_.reset();
    menu_assets_ = AssetRegistry::placeholders();
    disc_mounted_ = false;
    disc_region_ = disc_.empty() ? "NONE" : "UNKNOWN";
    if (disc_.empty()) {
        return;
    }
    auto mounted = mount_disc(disc_);
    if (!mounted) {
        return;
    }
    disc_region_ = mounted.value().identity.region == Region::Pal ? "PAL" : "JP";
    if (!disc_assets_) {
        return;
    }
    disc_mounted_ = true;
    menu_assets_ =
        AssetRegistry::from_disc(mounted.value(), false, flags_.asset_confidence, language_);
    for (const std::string& warning : menu_assets_.warnings()) {
        std::cerr << "oscilline: asset: " << warning << '\n';
    }
    disc_text_ = font_painter_for(menu_assets_);
}

void TitleScreen::sync_menu_audio() {
    const bool want = disc_assets_ && !disc_.empty();
    if (want) {
        if (audio_on_ && audio_disc_ == disc_) {
            return;
        }
        sfx_.load_path(disc_);
        if (ensure_menu_loop(sfx_.mixer())) {
            sfx_.discard();
        }
        audio_on_ = true;
        audio_disc_ = disc_;
        return;
    }
    if (!audio_on_) {
        return;
    }
    sfx_.mixer().stop(SfxId::MenuLoop);
    sfx_.load_path({});
    sfx_.mixer().drain();
    sfx_.discard();
    audio_on_ = false;
    audio_disc_.clear();
}

int TitleScreen::load_cached(const std::filesystem::path& file) {
    if (cached_ && cached_path_ == file && !music_key_.empty()) {
        music_ = file;
        return 1;
    }
    MusicLoadPump pump;
    pump.host = &host_;
    if (disc_text_) {
        pump.text = &*disc_text_;
    }
    auto decoded = decode_music_file(file, {}, pump_music_load, &pump);
    if (!pump.alive || (!decoded && decoded.error() == "closed")) {
        return -1;
    }
    if (!decoded) {
        notice_ = decoded.error();
        for (char& letter : notice_) {
            letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        }
        cached_.reset();
        cached_path_.clear();
        music_key_.clear();
        return 0;
    }
    music_ = file;
    cached_ = std::move(decoded.value());
    cached_path_ = file;
    music_key_ = cached_->fingerprint;
    if (auto loaded = load_music_scores(music_score_path())) {
        music_scores_ = std::move(loaded.value());
    }
    notice_.clear();
    return 1;
}

// Back on the title list, on the Play My Music row, after a failed load.
void TitleScreen::show_music_row() {
    clock_.page = MenuPage::Main;
    clock_.index = main_menu_index(MainItem::Music, play_ok(), disc_loaded());
    clock_.motion = MenuMotion::Idle;
}

int TitleScreen::run() {
    reload_presentation();
    sync_menu_audio();
    if (clock_.page == MenuPage::Music) {
        const int loaded = load_cached(music_);
        if (loaded < 0) {
            return 0;
        }
        if (loaded == 0) {
            show_music_row();
        }
    }
    timed_notice_ = notice_;
    while (host_.begin_frame()) {
        Frame frame;
        frame.steps = host_.sim_steps();
        ui_ms_ += std::llround(static_cast<double>(frame.steps) * host_.step_seconds() * 1000.0);
        tick_notice();
        const HostInput& input = host_.input();
        take_picked_files();
        reload_presentation();
        sync_menu_audio();

        frame.page = clock_.page;
        frame.model_slot = page_model_slot(clock_.page);
        if (menu_page_uses_model(clock_.page) &&
            menu_assets_.origin(frame.model_slot) == AssetOrigin::Disc) {
            if (const auto* names = menu_assets_.animation_names(frame.model_slot)) {
                frame.clips = menu_clips(*names, flags_.asset_confidence);
            }
        }
        if (calib_.active) {
            update_calibration(input, frame);
        } else if (rebind_action_ >= 0) {
            update_rebind(input);
        } else {
            update_menu(input, frame);
        }
        if (frame.page == MenuPage::Scores && clock_.page != MenuPage::Scores) {
            if (ensure_menu_loop(sfx_.mixer())) {
                sfx_.discard();
            }
        }
        if (frame.menu.intent == MenuIntent::Quit) {
            break;
        }
        apply_intent(frame);
        if (frame.refresh_presentation) {
            reload_presentation();
            sync_menu_audio();
        }
        draw(frame);

        if (!pending_.empty() && !open_pending_music()) {
            return 0;
        }
        if (frame.menu.intent == MenuIntent::LaunchCourse) {
            launch_course(frame.menu.wheel_row);
        } else if (frame.menu.intent == MenuIntent::LaunchMusic) {
            launch_music(frame.menu.music_row);
        }
    }
    return 0;
}

// A notice clears three seconds after it last changed.
void TitleScreen::tick_notice() {
    if (notice_ != timed_notice_) {
        timed_notice_ = notice_;
        notice_since_ms_ = ui_ms_;
    }
    if (!notice_.empty() && ui_ms_ - notice_since_ms_ >= 3000) {
        notice_.clear();
        timed_notice_.clear();
    }
}

void TitleScreen::take_picked_files() {
    std::string picked;
    std::string dialog_error;
    if (host_.take_disc_file(picked, dialog_error)) {
        if (!dialog_error.empty()) {
            std::cerr << "oscilline: " << dialog_error << '\n';
            notice_ = "DIALOG FAILED";
        } else if (!picked.empty()) {
            disc_ = picked;
            stored_.disc_path = disc_.string();
            save_stored();
            notice_ = "DISC SET";
        }
    }
    std::string music_picked;
    std::string music_error;
    if (host_.take_music_file(music_picked, music_error)) {
        if (!music_error.empty()) {
            std::cerr << "oscilline: " << music_error << '\n';
            notice_ = "DIALOG FAILED";
        } else if (!music_picked.empty()) {
            pending_ = music_picked;
        }
    }
}

void TitleScreen::update_calibration(const HostInput& input, Frame& frame) {
    const double frame_ms = static_cast<double>(frame.steps) * host_.step_seconds() * 1000.0;
    if (frame_ms > 0.0) {
        calib_.elapsed_ms += static_cast<std::int64_t>(frame_ms);
    }
    if (calib_.elapsed_ms >= calib_.next_click_ms) {
        while (calib_.elapsed_ms >= calib_.next_click_ms) {
            calib_.next_click_ms += kCalibrationIntervalMs;
        }
        calib_.last_click_ms = calib_.next_click_ms - kCalibrationIntervalMs;
        SfxNote note;
        note.pcm = click_.data();
        note.sample_count = static_cast<int>(click_.size());
        note.rate_hz = 44100;
        note.use_adsr = false;
        note.ducks_music = false;
        note.gain_l_q15 = 32767;
        note.gain_r_q15 = 32767;
        sfx_.mixer().post_note(note);
    }
    frame.flash =
        calib_.last_click_ms >= 0 && calib_.elapsed_ms - calib_.last_click_ms < kCalibrationFlashMs;
    const bool tap = pressed(input, Key::Confirm) || pressed(input, Key::Face) ||
                     pressed(input, Key::LeftShoulder) || pressed(input, Key::RightShoulder) ||
                     pressed(input, Key::Down) || pressed(input, Key::Block) ||
                     pressed(input, Key::Loop) || pressed(input, Key::Wave) ||
                     pressed(input, Key::Pit);
    if (tap && calib_.elapsed_ms >= kCalibrationIntervalMs) {
        const std::int64_t beat = nearest_click_ms(calib_.elapsed_ms, kCalibrationIntervalMs);
        calib_.samples.push_back(static_cast<int>(calib_.elapsed_ms - beat));
        effective_.playback.timing_offset_ms = average_timing_offset(calib_.samples);
    }
    if (static_cast<int>(calib_.samples.size()) >= kCalibrationTaps) {
        stored_.playback.timing_offset_ms = effective_.playback.timing_offset_ms;
        save_stored();
        calib_.active = false;
        notice_ = "AUDIO OFFSET SET";
    } else if (pressed(input, Key::Back)) {
        effective_.playback.timing_offset_ms = calib_.previous_offset;
        calib_.active = false;
    }
}

// Escape or the pad's Select cancels. Anything else binds the row's action.
void TitleScreen::update_rebind(const HostInput& input) {
    const auto action = static_cast<GameAction>(rebind_action_);
    bool done = false;
    bool bound = false;
    if (!input.key_name_pressed.empty()) {
        done = true;
        if (key_names_equal(input.key_name_pressed, "Escape")) {
            notice_.clear();
        } else if (bind_key(effective_.controls, action, input.key_name_pressed)) {
            bound = true;
        } else {
            notice_ = key_label(input.key_name_pressed) + " IS RESERVED";
        }
    } else if (input.pad_button_pressed >= 0 &&
               input.pad_button_pressed < static_cast<int>(PadButton::Count)) {
        done = true;
        const auto button = static_cast<PadButton>(input.pad_button_pressed);
        if (button == PadButton::Back) {
            notice_.clear();
        } else if (bind_pad(effective_.controls, action, button)) {
            bound = true;
        } else {
            notice_ = std::string(pad_button_label(button)) + " IS RESERVED";
        }
    }
    if (bound) {
        stored_.controls = effective_.controls;
        host_.set_controls(effective_.controls);
        save_stored();
        notice_ = std::string(game_action_label(action)) + " SET";
    }
    if (done) {
        rebind_action_ = -1;
    }
}

void TitleScreen::update_menu(const HostInput& input, Frame& frame) {
    MenuInput menu_input;
    if (pressed(input, Key::Up)) {
        menu_input.delta = -1;
    } else if (pressed(input, Key::Down)) {
        menu_input.delta = 1;
    }
    if (pressed(input, Key::Left)) {
        menu_input.horizontal = -1;
    } else if (pressed(input, Key::Right)) {
        menu_input.horizontal = 1;
    }
    menu_input.confirm =
        pressed(input, Key::Confirm) || pressed(input, Key::Face) || pressed(input, Key::Start);
    menu_input.back = pressed(input, Key::Back);
    // Keep the cursor on the same item when Play Original or Load Disc appears or goes.
    const std::pair<bool, bool> main_rows{play_ok(), disc_loaded()};
    if (clock_.page == MenuPage::Main && clock_.motion == MenuMotion::Idle && main_rows_was_ &&
        *main_rows_was_ != main_rows) {
        const MainItem held =
            main_menu_item(clock_.index, main_rows_was_->first, main_rows_was_->second);
        clock_.index = main_menu_index(held, play_ok(), disc_loaded());
    }
    frame.menu =
        menu_step(clock_,
                  menu_input,
                  frame.clips,
                  static_cast<float>(static_cast<double>(frame.steps) * host_.step_seconds()),
                  !music_.empty(),
                  play_ok(),
                  language_row_for(language_),
                  disc_loaded());
    if (clock_.page == MenuPage::Main) {
        main_rows_was_ = main_rows;
    }
    apply_settings_intent(frame);
    const MenuSfxCues cues = menu_sfx_cues(frame.page, frame.menu.action);
    for (int cue = 0; cue < cues.count; ++cue) {
        sfx_.post(cues.ids[cue]);
    }
    const auto sequence =
        menu_move_sequence(frame.page, frame.menu.action, menu_shown_index(clock_));
    if (!sequence.empty()) {
        sfx_.post_sequence(sequence);
    }
}

// Options rows, rebinding, the timing test, and the resets.
void TitleScreen::apply_settings_intent(Frame& frame) {
    MenuStep& menu = frame.menu;
    const bool option_step =
        (menu.action == MenuAction::Adjust && menu.option_row != kOptionsLanguage) ||
        menu.intent == MenuIntent::ToggleOption;
    if (option_step) {
        const int delta = menu.intent == MenuIntent::ToggleOption ? 0 : menu.adjust;
        const OptionChange change =
            change_option(menu.option_row, delta, effective_, stored_, host_, disc_assets_locked_);
        if (change == OptionChange::Changed) {
            if (menu.option_row == kOptionsDiscAssets) {
                disc_assets_ = effective_.disc_assets;
                frame.refresh_presentation = true;
            }
            sfx_.set_playback(playback_from(effective_));
            save_stored();
        } else {
            if (change == OptionChange::Locked) {
                notice_ = "SET BY --no-disc-assets";
            }
            menu.action = MenuAction::None;
        }
    } else if (menu.intent == MenuIntent::BeginRebind) {
        rebind_action_ = options_bind_action(menu.option_row);
        notice_.clear();
    } else if (menu.intent == MenuIntent::ResetControls) {
        effective_.controls = {};
        stored_.controls = {};
        host_.set_controls(effective_.controls);
        save_stored();
        notice_ = "CONTROLS RESET";
    } else if (menu.intent == MenuIntent::BeginCalibration) {
        calib_ = {};
        calib_.active = true;
        calib_.previous_offset = effective_.playback.timing_offset_ms;
    } else if (menu.intent == MenuIntent::ResetHighScores) {
        scores_ = {};
        music_scores_ = {};
        const auto course_saved = save_high_scores(high_score_path(), scores_);
        const auto music_saved = save_music_scores(music_score_path(), music_scores_);
        if (!course_saved || !music_saved) {
            std::cerr << "oscilline: could not reset all high scores\n";
            notice_ = "COULD NOT RESET HIGH SCORES";
        } else {
            notice_ = "HIGH SCORES RESET";
        }
    } else if (menu.intent == MenuIntent::ResetSettings) {
        effective_ = Settings{};
        stored_ = Settings{};
        disc_.clear();
        if (disc_assets_locked_) {
            effective_.disc_assets = false;
        }
        disc_assets_ = effective_.disc_assets;
        language_ = effective_.language_pak;
        host_.set_aspect_ratio(effective_.aspect_ratio);
        host_.set_classic_resolution(effective_.classic_resolution);
        effective_.window_scale = host_.set_window_scale(effective_.window_scale);
        stored_.window_scale = effective_.window_scale;
        host_.set_fullscreen(effective_.fullscreen);
        host_.set_controls(effective_.controls);
        sfx_.set_playback(playback_from(effective_));
        save_stored();
        frame.refresh_presentation = true;
        notice_ = "SETTINGS RESET";
    }
}

// File pickers and the language choice. Launches run after the frame is drawn.
void TitleScreen::apply_intent(Frame& frame) {
    const MenuStep& menu = frame.menu;
    if (menu.intent == MenuIntent::OpenDisc) {
        host_.choose_disc_file();
    } else if (menu.intent == MenuIntent::OpenMusic) {
        host_.choose_music_file();
    } else if (menu.intent == MenuIntent::SetLanguage) {
        const auto rows = language_rows();
        if (menu.language >= 0 && static_cast<std::size_t>(menu.language) < rows.size()) {
            language_ = rows[static_cast<std::size_t>(menu.language)].pak;
            stored_.language_pak = language_;
            save_stored();
            frame.refresh_presentation = true;
            notice_ = std::string(rows[static_cast<std::size_t>(menu.language)].label) + " SET";
        }
    }
}

void TitleScreen::draw(const Frame& frame) {
    const TextPainter& painter =
        disc_text_ ? static_cast<const TextPainter&>(*disc_text_) : debug_text_;
    painter.set_jitter_scale(text_shake_scale(effective_.text_shake));
    painter.set_time_ms(ui_ms_);

    TriangleList picture = title_mark();
    std::vector<TextGlyph> text;
    centered_glyph(picture, text, painter, 168.f, "OSCILLINE");
    Canvas canvas{picture, text, painter, menu_shown_index(clock_)};
    draw_models(canvas, frame);
    switch (clock_.page) {
    case MenuPage::Main:
        draw_main(canvas);
        break;
    case MenuPage::Wheel:
        draw_wheel(canvas);
        break;
    case MenuPage::Music:
        draw_music(canvas);
        break;
    case MenuPage::Language:
        draw_language(canvas);
        break;
    case MenuPage::Options:
        draw_options(canvas, frame);
        break;
    case MenuPage::About:
        draw_about(canvas);
        break;
    case MenuPage::Scores:
        draw_scores(canvas);
        break;
    }
    draw_footer(canvas);
    sfx_.pump();
    host_.end_frame(picture, text);
}

void TitleScreen::draw_models(Canvas& canvas, const Frame& frame) {
    if (menu_page_uses_model(clock_.page)) {
        ScreenRect model_rect;
        model_rect.left = 430.f;
        model_rect.top = 72.f;
        model_rect.right = 620.f;
        model_rect.bottom = 250.f;
        paint_slot(canvas.picture,
                   menu_assets_,
                   frame.model_slot,
                   menu_visual(clock_, frame.clips),
                   model_rect);
    }
    if ((clock_.page == MenuPage::Main || clock_.page == MenuPage::Wheel) &&
        menu_assets_.origin(Slot::MenuModel) == AssetOrigin::Disc) {
        MenuClipSet character = {};
        if (const auto* names = menu_assets_.animation_names(Slot::MenuModel)) {
            character = menu_clips(*names, flags_.asset_confidence);
        }
        MenuVisual idle;
        idle.clip = character.idle;
        idle.loop = true;
        idle.seconds = clock_.idle_seconds;
        ScreenRect character_rect;
        character_rect.left = 16.f;
        character_rect.top = 72.f;
        character_rect.right = 180.f;
        character_rect.bottom = 250.f;
        paint_slot(canvas.picture, menu_assets_, Slot::MenuModel, idle, character_rect, true);
    }
}

void TitleScreen::draw_main(Canvas& canvas) {
    const int rows = main_menu_count(play_ok(), disc_loaded());
    for (int row = 0; row < rows; ++row) {
        const float y = 200.f + static_cast<float>(row) * 16.f;
        menu_row(canvas.picture,
                 canvas.text,
                 canvas.painter,
                 248.f,
                 y,
                 canvas.shown == row,
                 std::string(main_menu_label(main_menu_item(row, play_ok(), disc_loaded()))));
    }
}

void TitleScreen::draw_wheel(Canvas& canvas) {
    for (int row = 0; row < kWheelSlots; ++row) {
        const float y = 200.f + static_cast<float>(row) * 16.f;
        menu_row(canvas.picture,
                 canvas.text,
                 canvas.painter,
                 248.f,
                 y,
                 canvas.shown == row,
                 std::string(kWheelLabel[row]));
    }
}

void TitleScreen::draw_music(Canvas& canvas) {
    glyph(canvas.picture, canvas.text, canvas.painter, 248.f, 188.f, "PLAY MY MUSIC");
    for (int row = 0; row < kMenuMusicCount; ++row) {
        const float y = 212.f + static_cast<float>(row) * 16.f;
        menu_row(canvas.picture,
                 canvas.text,
                 canvas.painter,
                 248.f,
                 y,
                 canvas.shown == row,
                 std::string(kMusicLabel[row]));
    }
    if (canvas.shown < kDifficultyCount && !music_key_.empty()) {
        const int best =
            music_best(music_scores_, music_key_, static_cast<Difficulty>(canvas.shown));
        glyph(canvas.picture,
              canvas.text,
              canvas.painter,
              248.f,
              312.f,
              "BEST " + std::to_string(best));
    }
    if (!music_.empty()) {
        centered_glyph(canvas.picture,
                       canvas.text,
                       canvas.painter,
                       336.f,
                       fit(music_.filename().string(), 70));
    }
}

void TitleScreen::draw_language(Canvas& canvas) {
    glyph(canvas.picture, canvas.text, canvas.painter, 248.f, 188.f, "LANGUAGE");
    const auto rows = language_rows();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const float y = 212.f + static_cast<float>(row) * 16.f;
        menu_row(canvas.picture,
                 canvas.text,
                 canvas.painter,
                 248.f,
                 y,
                 canvas.shown == row,
                 std::string(rows[static_cast<std::size_t>(row)].label));
    }
}

void TitleScreen::draw_options(Canvas& canvas, const Frame& frame) {
    const TextPainter& painter = canvas.painter;
    // Tabs are measured and centered with room for the selector arrow between them.
    constexpr float kCategoryGap = 28.f;
    float tabs_width = kCategoryGap * static_cast<float>(kOptionsCategoryCount - 1);
    for (int category = 0; category < kOptionsCategoryCount; ++category) {
        tabs_width += painter.measure_width(options_category_name(category));
    }
    const float center_offset =
        (static_cast<float>(logical_width()) - static_cast<float>(kLogicalWidth)) * 0.5f;
    float tab_x = (static_cast<float>(kLogicalWidth) - tabs_width) * 0.5f;
    for (int category = 0; category < kOptionsCategoryCount; ++category) {
        const char* const label = options_category_name(category);
        const float label_width = painter.measure_width(label);
        const float label_x = tab_x;
        tab_x += label_width + kCategoryGap;
        glyph(canvas.picture, canvas.text, painter, label_x, 188.f, label);
        if (category == clock_.options_category) {
            if (clock_.options_tabs_focused) {
                paint_menu_selector(canvas.picture, label_x + center_offset - 14.f, 188.f);
            }
            std::vector<Segment> underline;
            const float y = 205.f;
            Segment segment;
            segment.x0 = label_x + center_offset;
            segment.y0 = y;
            segment.x1 = segment.x0 + label_width;
            segment.y1 = y;
            underline.push_back(segment);
            append_strokes(canvas.picture, underline);
        }
    }
    for (int row = 0; row < options_category_row_count(clock_.options_category); ++row) {
        const int setting_row = options_category_setting_row(clock_.options_category, row);
        std::string line =
            option_value(setting_row, effective_, disc_, disc_assets_, language_, disc_region_);
        if (rebind_action_ >= 0 && options_bind_action(setting_row) == rebind_action_) {
            line = std::string(game_action_label(static_cast<GameAction>(rebind_action_))) +
                   ":  PRESS A KEY OR BUTTON";
        }
        if (calib_.active && setting_row == kOptionsTiming) {
            line = "AUDIO OFFSET:  " + offset_text(effective_.playback.timing_offset_ms) + "  " +
                   std::to_string(calib_.samples.size()) + "/" + std::to_string(kCalibrationTaps);
        }
        const float y = 222.f + static_cast<float>(row) * 24.f;
        menu_row(canvas.picture,
                 canvas.text,
                 painter,
                 248.f,
                 y,
                 !clock_.options_tabs_focused && canvas.shown == row,
                 fit(std::move(line), 42));
    }
    if (calib_.active) {
        glyph(canvas.picture, canvas.text, painter, 248.f, 454.f, "TAP TO THE CLICK");
        if (frame.flash) {
            flash_marker(canvas.picture);
        }
    }
}

void TitleScreen::draw_about(Canvas& canvas) {
    const TextPainter& painter = canvas.painter;
    centered_glyph(canvas.picture, canvas.text, painter, 202.f, "By Marioaddict3");
    centered_glyph(canvas.picture, canvas.text, painter, 226.f, "ko-fi.com/marioaddict3");
    paint_kofi_qr(canvas.picture, 246.f);
    const float back_x =
        (static_cast<float>(kLogicalWidth) - painter.measure_width("    BACK")) * 0.5f;
    menu_row(canvas.picture, canvas.text, painter, back_x, 398.f, canvas.shown == 0, "BACK");
    const std::string version = "v0.1.1";
    const float version_x = static_cast<float>(kLogicalWidth) - kMenuFrame - kFooterPad -
                            painter.measure_width(version);
    glyph(canvas.picture, canvas.text, painter, version_x, kFooterBottom, version);
}

void TitleScreen::draw_scores(Canvas& canvas) {
    glyph(canvas.picture, canvas.text, canvas.painter, 280.f, 200.f, "SCORES");
    for (int slot = 0; slot < kDifficultyCount; ++slot) {
        const auto name = difficulty_name(static_cast<Difficulty>(slot));
        std::string line(name);
        for (char& letter : line) {
            letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        }
        line += " " + std::to_string(scores_.best[slot]);
        const float y = 224.f + static_cast<float>(slot) * 16.f;
        menu_row(canvas.picture, canvas.text, canvas.painter, 248.f, y, canvas.shown == slot, line);
    }
    menu_row(canvas.picture,
             canvas.text,
             canvas.painter,
             248.f,
             288.f,
             canvas.shown == kDifficultyCount,
             "  BACK");
}

void TitleScreen::draw_footer(Canvas& canvas) {
    if (clock_.page != MenuPage::About && disc_.empty() && music_.empty()) {
        glyph(
            canvas.picture, canvas.text, canvas.painter, kFooterX, kFooterBottom - 32.f, "NO DISC");
        glyph(canvas.picture,
              canvas.text,
              canvas.painter,
              kFooterX,
              kFooterBottom - 16.f,
              "PASS --disc <cue> OR USE LOAD DISC");
    }
    if (!notice_.empty()) {
        glyph(
            canvas.picture, canvas.text, canvas.painter, kFooterX, kFooterBottom, fit(notice_, 70));
    }
}

bool TitleScreen::open_pending_music() {
    const std::filesystem::path file = std::exchange(pending_, {});
    const int loaded = load_cached(file);
    if (loaded < 0) {
        return false;
    }
    if (loaded == 0) {
        show_music_row();
    } else {
        clock_.page = MenuPage::Music;
        clock_.index = 0;
        clock_.motion = MenuMotion::Idle;
        notice_ = "MUSIC SET";
    }
    return true;
}

void TitleScreen::launch_course(int wheel_row) {
    const auto difficulty = wheel_difficulty(wheel_row);
    if (!difficulty) {
        return;
    }
    if (disc_.empty()) {
        notice_ = "NO DISC";
        return;
    }
    const int slot = static_cast<int>(difficulty.value());
    const int old_best = scores_.best[slot];
    enter_play();
    const int played = run_difficulty(
        host_, play_options(effective_, disc_, language_, flags_), difficulty.value(), scores_);
    leave_play();
    if (scores_.best[slot] > old_best) {
        if (auto saved = save_high_scores(high_score_path(), scores_); !saved) {
            std::cerr << "oscilline: " << saved.error() << '\n';
        }
    }
    if (played != 0) {
        notice_ = "COULD NOT OPEN THAT DISC";
    } else {
        notice_.clear();
    }
}

void TitleScreen::launch_music(int row) {
    if (music_.empty() || row < 0 || row >= kDifficultyCount) {
        notice_ = "NO MUSIC";
        return;
    }
    const auto difficulty = static_cast<Difficulty>(row);
    const DecodedMusic* ready = cached_ && cached_path_ == music_ ? &*cached_ : nullptr;
    enter_play();
    const int played = run_music(
        host_, play_options(effective_, disc_, language_, flags_), music_, difficulty, ready);
    leave_play();
    if (played != 0) {
        notice_ = "COULD NOT PLAY THAT MUSIC";
    } else if (auto loaded = load_music_scores(music_score_path())) {
        music_scores_ = std::move(loaded.value());
        notice_.clear();
    }
}

} // namespace

int run_title(Host& host,
              TitleStart start,
              Settings effective,
              Settings stored,
              std::string_view language_pak,
              const RunFlags& flags,
              bool disc_assets_locked) {
    TitleScreen screen(host,
                       std::move(start),
                       std::move(effective),
                       std::move(stored),
                       language_pak,
                       flags,
                       disc_assets_locked);
    return screen.run();
}

} // namespace oscilline
