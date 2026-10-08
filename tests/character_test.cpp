// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Clip confidence, whiffs, and the disc-figure placement.

#include "oscilline/asset/character.hpp"
#include "oscilline/asset/menu.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/course/figure.hpp"
#include "oscilline/course/obstacle.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/render/project.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Buf {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value));
        u16(static_cast<std::uint16_t>(value >> 16));
    }

    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
};

std::vector<std::uint8_t> vertical_tmd() {
    Buf file;
    file.u32(0x41);
    file.u32(0);
    file.u32(1);
    file.u32(0x1C);
    file.u32(2);
    file.u32(0);
    file.u32(0);
    file.u32(0x2C);
    file.u32(1);
    file.i32(0);
    file.i16(0);
    file.i16(0);
    file.i16(0);
    file.u16(0);
    file.i16(0);
    file.i16(-100);
    file.i16(0);
    file.u16(0);
    file.u8(3);
    file.u8(2);
    file.u8(1);
    file.u8(0x40);
    file.u32(0x00FFFFFF);
    file.u16(0);
    file.u16(1);
    return file.bytes;
}

std::vector<std::uint8_t> still_anm() {
    Buf file;
    file.u16(0x8000);
    file.u16(30);
    file.u16(1);
    file.u16(5);
    file.u16(6);
    file.u8(0);
    file.u8(0);
    return file.bytes;
}

oscilline::AnmFile position_clip(std::int16_t rate, std::int16_t from, std::int16_t to) {
    oscilline::AnmFile clip;
    clip.unk1 = rate;
    clip.frame_count = 2;
    clip.frames.resize(2);
    for (int frame = 0; frame < 2; ++frame) {
        oscilline::AnmKeyframe key;
        key.object_index = 0;
        key.has_position = true;
        key.position_x = frame == 0 ? from : to;
        clip.frames[static_cast<std::size_t>(frame)].keys.push_back(key);
    }
    return clip;
}

oscilline::TmdObject line_object(std::int16_t x0,
                                 std::int16_t y0,
                                 std::int16_t z0,
                                 std::int16_t x1,
                                 std::int16_t y1,
                                 std::int16_t z1) {
    oscilline::TmdObject object;
    object.vertices.push_back({x0, y0, z0});
    object.vertices.push_back({x1, y1, z1});
    oscilline::TmdPrimitive line;
    line.kind = oscilline::PrimitiveKind::Line;
    line.vertex_indices = {0, 1};
    object.primitives.push_back(line);
    return object;
}

void expect_segment(const oscilline::Segment& segment, float x0, float y0, float x1, float y1) {
    CHECK(segment.x0 == doctest::Approx(x0));
    CHECK(segment.y0 == doctest::Approx(y0));
    CHECK(segment.x1 == doctest::Approx(x1));
    CHECK(segment.y1 == doctest::Approx(y1));
    CHECK(segment.depth == doctest::Approx(0.f));
}

oscilline::AssetRegistry registry_with(std::vector<oscilline::PakEntry> entries,
                                       bool force_placeholder,
                                       oscilline::Confidence minimum) {
    oscilline::PakArchive archive;
    archive.entries = std::move(entries);
    oscilline::AssetRegistry::Parts parts;
    parts.game = &archive;
    parts.game_pak_path = "GAME/02_FILES.PAK";
    parts.force_placeholder = force_placeholder;
    parts.minimum = minimum;
    return oscilline::AssetRegistry::from_parts(parts);
}

oscilline::PakEntry named_anm(const char* name) {
    oscilline::PakEntry entry;
    entry.name = name;
    entry.data = still_anm();
    return entry;
}

int clip_index(const std::vector<std::string>& names, std::string_view stem) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].find(stem) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("character clip tokens name the evidence and the confidence") {
    bool saw_block = false;
    bool saw_loop_letter = false;
    bool saw_miss = false;
    for (const oscilline::ClipToken& token : oscilline::character_clip_tokens()) {
        CHECK(token.evidence_note != nullptr);
        CHECK(token.evidence_note[0] != '\0');
        if (std::string(token.text) == "B") {
            saw_block = true;
            CHECK(token.actions == oscilline::kActionBlock);
            CHECK(token.confidence == oscilline::Confidence::High);
        }
        if (std::string(token.text) == "P") {
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.actions == oscilline::kActionPit);
        }
        if (std::string(token.text) == "L") {
            saw_loop_letter = true;
            CHECK(token.actions == oscilline::kActionLoop);
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.evidence == oscilline::Evidence::Emulator);
        }
        if (std::string(token.text) == "W") {
            CHECK(token.actions == oscilline::kActionWave);
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.evidence == oscilline::Evidence::Emulator);
        }
        if (std::string(token.text) == "MISS") {
            saw_miss = true;
            CHECK(token.kind == oscilline::ClipKind::Miss);
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.evidence == oscilline::Evidence::Visual);
        }
        if (std::string(token.text) == "SKIP") {
            CHECK(token.kind == oscilline::ClipKind::Idle);
            CHECK(token.confidence == oscilline::Confidence::High);
        }
        if (std::string(token.text) == "J") {
            CHECK(token.actions == oscilline::kActionBlock);
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.evidence == oscilline::Evidence::Emulator);
        }
        if (std::string(token.text) == "H") {
            CHECK(token.actions == oscilline::kActionPit);
            CHECK(token.confidence == oscilline::Confidence::High);
            CHECK(token.evidence == oscilline::Evidence::Visual);
            CHECK(oscilline::evidence_name(token.evidence) == "visual");
        }
    }
    CHECK(saw_block);
    CHECK(saw_loop_letter);
    CHECK(saw_miss);

    const auto block = oscilline::match_character_clip("CHARA/PEELOO/B.ANM");
    CHECK(block.matched);
    CHECK(block.kind == oscilline::ClipKind::Action);
    CHECK(block.actions == oscilline::kActionBlock);
    CHECK(block.confidence == oscilline::Confidence::High);
    const auto pit = oscilline::match_character_clip("block_pit.anm");
    CHECK(pit.actions == (oscilline::kActionBlock | oscilline::kActionPit));
    CHECK(pit.confidence == oscilline::Confidence::High);
    const auto swapped = oscilline::match_character_clip("PB.ANM");
    CHECK(swapped.actions == (oscilline::kActionBlock | oscilline::kActionPit));
    const auto loop_letter = oscilline::match_character_clip("L.ANM");
    CHECK(loop_letter.confidence == oscilline::Confidence::High);
    CHECK(loop_letter.actions == oscilline::kActionLoop);
    const auto wave_letter = oscilline::match_character_clip("W.ANM");
    CHECK(wave_letter.confidence == oscilline::Confidence::High);
    CHECK(wave_letter.actions == oscilline::kActionWave);
    const auto loop_word = oscilline::match_character_clip("LOOP.ANM");
    CHECK(loop_word.confidence == oscilline::Confidence::High);
    const auto wave_word = oscilline::match_character_clip("WAVE.ANM");
    CHECK(wave_word.confidence == oscilline::Confidence::High);
    CHECK(wave_word.actions == oscilline::kActionWave);
    const auto waited = oscilline::match_character_clip("WAIT.ANM");
    CHECK(waited.kind == oscilline::ClipKind::Idle);
    CHECK(waited.confidence == oscilline::Confidence::High);
    CHECK_FALSE(oscilline::match_character_clip("LEFT.ANM").matched);
    CHECK_FALSE(oscilline::match_character_clip("N08_SUPER.ANM").matched);
    const auto idle = oscilline::match_character_clip("IDLE.ANM");
    CHECK(idle.kind == oscilline::ClipKind::Idle);
    CHECK(idle.confidence == oscilline::Confidence::High);
    const auto miss = oscilline::match_character_clip("B_MISS.ANM");
    CHECK(miss.kind == oscilline::ClipKind::Miss);
    CHECK(miss.confidence == oscilline::Confidence::High);
    CHECK(miss.actions == oscilline::kActionBlock);
    CHECK_FALSE(oscilline::match_character_clip("NOISE.ANM").matched);

    const auto skip = oscilline::match_character_clip("CHARA/PEELOO/N00_SKIP.ANM");
    CHECK(skip.kind == oscilline::ClipKind::Idle);
    CHECK(skip.confidence == oscilline::Confidence::High);
    const auto skip_variant = oscilline::match_character_clip("N00_SKIP_F.ANM");
    CHECK(skip_variant.kind == oscilline::ClipKind::Idle);
    CHECK(skip_variant.confidence == oscilline::Confidence::High);
    const auto jump = oscilline::match_character_clip("N01_J.ANM");
    CHECK(jump.kind == oscilline::ClipKind::Action);
    CHECK(jump.actions == oscilline::kActionBlock);
    CHECK(jump.confidence == oscilline::Confidence::High);
    const auto hop = oscilline::match_character_clip("N02_H.ANM");
    CHECK(hop.actions == oscilline::kActionPit);
    CHECK(hop.confidence == oscilline::Confidence::High);
    const auto block_whiff = oscilline::match_character_clip("N01_J_F.ANM");
    CHECK(block_whiff.whiff);
    CHECK(block_whiff.actions == oscilline::kActionBlock);
    CHECK(block_whiff.confidence == oscilline::Confidence::High);
    const auto loop_whiff = oscilline::match_character_clip("N03_L_F.ANM");
    CHECK(loop_whiff.whiff);
    CHECK(loop_whiff.actions == oscilline::kActionLoop);
    CHECK(loop_whiff.confidence == oscilline::Confidence::High);
    const auto numbered_loop = oscilline::match_character_clip("N03_L.ANM");
    CHECK(numbered_loop.actions == oscilline::kActionLoop);
    CHECK(numbered_loop.confidence == oscilline::Confidence::High);
    const auto numbered_wave = oscilline::match_character_clip("N04_W.ANM");
    CHECK(numbered_wave.actions == oscilline::kActionWave);
    CHECK(numbered_wave.confidence == oscilline::Confidence::High);
    const auto wave_variant = oscilline::match_character_clip("N04_W_F.ANM");
    CHECK(wave_variant.whiff);
    CHECK(wave_variant.actions == oscilline::kActionWave);
    CHECK(wave_variant.confidence == oscilline::Confidence::High);
    const auto pair = oscilline::match_character_clip("N05_J_H.ANM");
    CHECK(pair.actions == (oscilline::kActionBlock | oscilline::kActionPit));
    CHECK(pair.confidence == oscilline::Confidence::High);
    const auto pair_variant = oscilline::match_character_clip("N06_JH_F.ANM");
    CHECK(pair_variant.actions == (oscilline::kActionBlock | oscilline::kActionPit));
    CHECK(pair_variant.confidence == oscilline::Confidence::High);
    const auto loop_wave = oscilline::match_character_clip("N06_LW.ANM");
    CHECK(loop_wave.actions == (oscilline::kActionLoop | oscilline::kActionWave));
    CHECK(loop_wave.confidence == oscilline::Confidence::High);
    const auto mixed_pair = oscilline::match_character_clip("N07_JL.ANM");
    CHECK(mixed_pair.actions == (oscilline::kActionBlock | oscilline::kActionLoop));
    CHECK(mixed_pair.confidence == oscilline::Confidence::High);
}

TEST_CASE("form folders stay on the asset map and super uses the upgraded folder") {
    const auto base = oscilline::form_model_slot(oscilline::Form::Rabbit);
    const auto frog = oscilline::form_model_slot(oscilline::Form::Frog);
    const auto worm = oscilline::form_model_slot(oscilline::Form::Worm);
    const auto super = oscilline::form_model_slot(oscilline::Form::Super);
    REQUIRE(base);
    REQUIRE(frog);
    REQUIRE(worm);
    REQUIRE(super);
    CHECK(*base == oscilline::Slot::FormBase);
    CHECK(*frog == oscilline::Slot::FormDegradedA);
    CHECK(*worm == oscilline::Slot::FormDegradedB);
    CHECK(*super == oscilline::Slot::FormUpgraded);
    CHECK(std::string(oscilline::map_row(*base)->pattern) == "CHARA/PEELOO");
    CHECK(std::string(oscilline::map_row(*frog)->pattern) == "CHARA/FROG");
    CHECK(std::string(oscilline::map_row(*worm)->pattern) == "CHARA/SNAKE");
    CHECK(std::string(oscilline::map_row(*super)->pattern) == "CHARA/SUPER");
    CHECK(oscilline::map_row(*base)->confidence == oscilline::Confidence::High);
    CHECK(oscilline::map_row(*super)->confidence == oscilline::Confidence::High);
    CHECK_FALSE(oscilline::form_model_slot(oscilline::Form::Out));
    CHECK(*base != oscilline::Slot::FormUpgraded);
    CHECK(*frog != oscilline::Slot::FormUpgraded);
    CHECK(*worm != oscilline::Slot::FormUpgraded);
}

TEST_CASE("the animation clock follows input and does not change a judgment") {
    const std::vector<std::string> names = {"IDLE.ANM", "B.ANM", "L.ANM", "BP.ANM", "MISS.ANM"};
    const auto high = oscilline::character_library(names, oscilline::Confidence::High);
    CHECK(high.usable);
    CHECK(high.idle == 0);
    CHECK(high.action[oscilline::kActionBlock] == 1);
    CHECK(high.action[oscilline::kActionBlock | oscilline::kActionPit] == 3);
    CHECK(high.action[oscilline::kActionLoop] == 2);
    CHECK(high.miss[0] == 4);

    std::vector<oscilline::AnmFile> anims(names.size());
    anims[0].unk1 = 30;
    anims[0].frames.resize(30);
    anims[1].unk1 = 10;
    anims[1].frames.resize(5);
    anims[3].unk1 = 10;
    anims[3].frames.resize(4);
    anims[4].unk1 = 10;
    anims[4].frames.resize(3);

    oscilline::CharacterClock clock;
    oscilline::character_advance(clock, high, anims, 0.2, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 0);
    CHECK_FALSE(clock.oneshot);

    oscilline::character_advance(
        clock, high, anims, 0.0, oscilline::kActionBlock, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 1);
    CHECK(clock.seconds == doctest::Approx(0));
    CHECK(clock.oneshot);
    oscilline::character_advance(clock, high, anims, 0.49, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 1);
    oscilline::character_advance(clock, high, anims, 0.02, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 0);
    CHECK_FALSE(clock.oneshot);

    oscilline::character_advance(
        clock, high, anims, 0.0, oscilline::kActionLoop, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 2);
    oscilline::character_advance(clock, high, anims, 0.0, 0, true, oscilline::Form::Rabbit);
    CHECK(clock.clip == 4);
    CHECK(clock.miss);

    oscilline::character_advance(
        clock,
        high,
        anims,
        0.0,
        static_cast<std::uint8_t>(oscilline::kActionBlock | oscilline::kActionPit),
        false,
        oscilline::Form::Rabbit);
    CHECK(clock.clip == 3);

    const auto medium = oscilline::character_library(names, oscilline::Confidence::Medium);
    CHECK(medium.action[oscilline::kActionLoop] == 2);
    CHECK(medium.action[oscilline::kActionBlock | oscilline::kActionPit] == 3);
    CHECK(medium.miss[0] == 4);
    oscilline::CharacterClock medium_clock;
    anims[2].unk1 = 10;
    anims[2].frames.resize(4);
    oscilline::character_advance(
        medium_clock, medium, anims, 0.0, oscilline::kActionLoop, false, oscilline::Form::Rabbit);
    CHECK(medium_clock.clip == 2);

    const auto low = oscilline::character_library(names, oscilline::Confidence::Low);
    CHECK(low.miss[0] == 4);
    oscilline::CharacterClock miss_clock;
    oscilline::character_advance(miss_clock, low, anims, 0.0, 0, true, oscilline::Form::Rabbit);
    CHECK(miss_clock.miss);
    CHECK(miss_clock.clip == 4);

    oscilline::CharacterLibrary frog_library;
    frog_library.usable = true;
    frog_library.idle = 0;
    oscilline::AnmFile frog_idle;
    frog_idle.unk1 = 30;
    frog_idle.frames.resize(4);
    const std::vector<oscilline::AnmFile> frog_anims{frog_idle};
    oscilline::character_advance(
        clock, frog_library, frog_anims, 0.1, oscilline::kActionBlock, true, oscilline::Form::Frog);
    CHECK(clock.form == oscilline::Form::Frog);
    CHECK(clock.clip == 0);
    CHECK(clock.seconds == doctest::Approx(0));
    CHECK_FALSE(clock.oneshot);

    oscilline::CharacterClock promote;
    oscilline::character_advance(promote, high, anims, 0.4, 0, false, oscilline::Form::Rabbit);
    CHECK(promote.clip == 0);
    CHECK(promote.seconds == doctest::Approx(0.4));
    oscilline::CharacterClock fast;
    oscilline::character_advance(fast,
                                 high,
                                 anims,
                                 0.2,
                                 0,
                                 false,
                                 oscilline::Form::Rabbit,
                                 oscilline::ClipMoment::None,
                                 2.0);
    CHECK(fast.seconds == doctest::Approx(0.4));
    oscilline::character_advance(
        promote, high, anims, 0.05, 0, false, oscilline::Form::Super, oscilline::ClipMoment::None);
    CHECK(promote.form == oscilline::Form::Super);
    CHECK(promote.clip == 0);
    CHECK(promote.seconds == doctest::Approx(0.45));
    CHECK_FALSE(promote.oneshot);
    oscilline::character_advance(promote,
                                 high,
                                 anims,
                                 0.0,
                                 oscilline::kActionBlock,
                                 false,
                                 oscilline::Form::Super,
                                 oscilline::ClipMoment::Clear);
    CHECK(promote.clip == 1);
    CHECK(promote.seconds == doctest::Approx(0));
    CHECK(promote.oneshot);

    oscilline::CourseTimeline course;
    course.duration_ms = 5000;
    oscilline::CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 1000;
    event.approach_ms = 1000;
    event.scroll_approach_ms = 1000;
    course.events.push_back(event);
    oscilline::PlayState play;
    const oscilline::ObstacleWindow window = oscilline::obstacle_window(event);
    const std::int64_t when = (window.perfect_open + window.perfect_close) / 2;
    oscilline::play_advance(play, course, window.good_open, when, oscilline::kActionBlock);
    const int score = play.score;
    const oscilline::Form form = play.form;
    oscilline::character_advance(
        clock, high, anims, 1.0, oscilline::kActionBlock, false, play.form);
    CHECK(play.score == score);
    CHECK(play.form == form);
    CHECK(play.score == 3);
}

TEST_CASE("a generated model poses, blends, and plants its feet on the ribbon") {
    oscilline::TmdModel model;
    model.objects.push_back(line_object(0, 0, 0, 0, -10, 0));
    model.objects.push_back(line_object(0, -4, -2, 0, -4, 2));
    model.objects.push_back(line_object(3, 0, 0, 4, 0, 0));
    oscilline::AnmFile idle = position_clip(30, 0, 10);
    oscilline::AnmKeyframe arm;
    arm.object_index = 1;
    arm.has_position = true;
    oscilline::AnmFile action;
    action.unk1 = 10;
    action.frame_count = 2;
    action.frames.resize(2);
    action.frames[0].keys.push_back(idle.frames[0].keys[0]);
    action.frames[0].keys.push_back(arm);
    action.frames[1].keys.push_back(idle.frames[1].keys[0]);
    action.frames[1].keys.push_back(arm);

    const auto mid = oscilline::poses_at_time(idle, 3, 0.5 / 30.0, true);
    REQUIRE(mid.size() == 3);
    CHECK(mid[0].visible);
    CHECK(mid[0].position_x == doctest::Approx(5.f));
    CHECK_FALSE(mid[1].visible);
    CHECK_FALSE(mid[2].visible);
    CHECK(oscilline::tmd_wireframe(model, mid).size() == 1);

    const auto posed = oscilline::poses_at_time(action, 3, 0, false);
    CHECK(posed[0].visible);
    CHECK(posed[1].visible);
    CHECK_FALSE(posed[2].visible);
    CHECK(oscilline::tmd_wireframe(model, posed).size() == 2);

    const std::vector<std::string> names = {"IDLE.ANM", "B.ANM"};
    const auto library = oscilline::character_library(names, oscilline::Confidence::High);
    std::vector<oscilline::AnmFile> anims{idle, action};
    oscilline::CharacterClock clock;
    oscilline::character_advance(
        clock, library, anims, 0, oscilline::kActionBlock, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 1);
    oscilline::character_advance(clock, library, anims, 0.2, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == 0);

    oscilline::PakEntry model_entry;
    model_entry.name = "CHARA/PEELOO/MODEL.TMD";
    model_entry.data = vertical_tmd();
    oscilline::PakEntry idle_entry;
    idle_entry.name = "CHARA/PEELOO/IDLE.ANM";
    idle_entry.data = still_anm();
    oscilline::PakEntry block_entry;
    block_entry.name = "CHARA/PEELOO/B.ANM";
    block_entry.data = still_anm();
    oscilline::PakEntry loop_entry;
    loop_entry.name = "CHARA/PEELOO/L.ANM";
    loop_entry.data = still_anm();
    auto registry = registry_with(
        {model_entry, idle_entry, block_entry, loop_entry}, false, oscilline::Confidence::High);
    auto rig = oscilline::make_character_rig(registry, oscilline::Form::Rabbit);
    CHECK(rig.disc);
    CHECK(rig.placement.valid);
    CHECK(rig.library.action[oscilline::kActionBlock] >= 0);
    CHECK(rig.library.action[oscilline::kActionLoop] >= 0);
    const auto* clips = registry.animations(oscilline::Slot::FormBase);
    REQUIRE(clips != nullptr);
    REQUIRE(rig.library.idle >= 0);
    oscilline::DiscFigurePose pose;
    pose.model = registry.model(oscilline::Slot::FormBase);
    pose.clip = &(*clips)[static_cast<std::size_t>(rig.library.idle)];
    pose.loop = true;
    pose.placement = rig.placement;
    std::vector<oscilline::Segment> drawn;
    CHECK(oscilline::paint_disc_figure(drawn, pose, 120.f, 240.f));
    REQUIRE(drawn.size() == 1);
    const float foot_y = std::max(drawn[0].y0, drawn[0].y1);
    const float head_y = std::min(drawn[0].y0, drawn[0].y1);
    CHECK(foot_y == doctest::Approx(240.f - oscilline::kFigureRibbonClearancePx));
    CHECK(head_y == doctest::Approx(240.f - oscilline::kFigureRibbonClearancePx -
                                    oscilline::kFigureHeightPx));
    CHECK(drawn[0].x0 == doctest::Approx(120.f));
    CHECK(drawn[0].x1 == doctest::Approx(120.f));

    auto hidden = registry_with({model_entry, idle_entry}, true, oscilline::Confidence::High);
    CHECK_FALSE(oscilline::make_character_rig(hidden, oscilline::Form::Rabbit).disc);
    auto nameless = registry_with({model_entry, block_entry}, false, oscilline::Confidence::High);
    CHECK_FALSE(oscilline::make_character_rig(nameless, oscilline::Form::Rabbit).disc);
    auto placeholders = oscilline::AssetRegistry::placeholders();
    CHECK_FALSE(oscilline::make_character_rig(placeholders, oscilline::Form::Frog).disc);
}

TEST_CASE("numbered clip stems enable the disc figure at default confidence") {
    const std::vector<std::string> names = {
        "N00_SKIP.ANM", "N01_J.ANM", "N02_H.ANM", "N03_L.ANM", "N04_W.ANM", "N05_JH.ANM"};
    const auto high = oscilline::character_library(names, oscilline::Confidence::High);
    CHECK(high.usable);
    CHECK(high.idle == 0);
    CHECK(high.action[oscilline::kActionBlock] == 1);
    CHECK(high.action[oscilline::kActionPit] == 2);
    CHECK(high.action[oscilline::kActionLoop] == 3);
    CHECK(high.action[oscilline::kActionWave] == 4);
    CHECK(high.action[oscilline::kActionBlock | oscilline::kActionPit] == 5);
    const auto medium = oscilline::character_library(names, oscilline::Confidence::Medium);
    CHECK(medium.usable);
    CHECK(medium.idle == 0);
    CHECK(medium.action[oscilline::kActionBlock] == 1);
    CHECK(medium.action[oscilline::kActionPit] == 2);
    CHECK(medium.action[oscilline::kActionLoop] == 3);
    CHECK(medium.action[oscilline::kActionWave] == 4);
    CHECK(medium.action[oscilline::kActionBlock | oscilline::kActionPit] == 5);

    oscilline::PakEntry model_entry;
    model_entry.name = "CHARA/PEELOO/MODEL.TMD";
    model_entry.data = vertical_tmd();
    oscilline::PakEntry idle_entry;
    idle_entry.name = "CHARA/PEELOO/N00_SKIP.ANM";
    idle_entry.data = still_anm();
    oscilline::PakEntry block_entry;
    block_entry.name = "CHARA/PEELOO/N01_J.ANM";
    block_entry.data = still_anm();
    auto registry =
        registry_with({model_entry, idle_entry, block_entry}, false, oscilline::Confidence::High);
    auto rig = oscilline::make_character_rig(registry, oscilline::Form::Rabbit);
    CHECK(rig.disc);
    CHECK(rig.library.usable);
    CHECK(rig.library.action[oscilline::kActionBlock] >= 0);
    const auto* clips = registry.animations(oscilline::Slot::FormBase);
    REQUIRE(clips != nullptr);
    REQUIRE(rig.library.idle >= 0);
    oscilline::DiscFigurePose pose;
    pose.model = registry.model(oscilline::Slot::FormBase);
    pose.clip = &(*clips)[static_cast<std::size_t>(rig.library.idle)];
    pose.loop = true;
    pose.placement = rig.placement;
    std::vector<oscilline::Segment> drawn;
    CHECK(oscilline::paint_disc_figure(drawn, pose, 120.f, 240.f));
    REQUIRE(drawn.size() == 1);
    CHECK(std::max(drawn[0].y0, drawn[0].y1) ==
          doctest::Approx(240.f - oscilline::kFigureRibbonClearancePx));
}

TEST_CASE("numbered clears play at the default confidence when the row is high") {
    oscilline::PakEntry model_entry;
    model_entry.name = "CHARA/PEELOO/MODEL.TMD";
    model_entry.data = vertical_tmd();
    auto registry = registry_with({model_entry,
                                   named_anm("CHARA/PEELOO/N00_SKIP.ANM"),
                                   named_anm("CHARA/PEELOO/N01_J.ANM"),
                                   named_anm("CHARA/PEELOO/N02_H.ANM"),
                                   named_anm("CHARA/PEELOO/N03_L.ANM"),
                                   named_anm("CHARA/PEELOO/N04_W.ANM"),
                                   named_anm("CHARA/PEELOO/N05_JH.ANM"),
                                   named_anm("CHARA/PEELOO/N06_LW.ANM"),
                                   named_anm("CHARA/PEELOO/N07_JH_F.ANM"),
                                   named_anm("CHARA/PEELOO/N08_SKIP_F.ANM"),
                                   named_anm("CHARA/PEELOO/N09_B_MISS.ANM"),
                                   named_anm("CHARA/PEELOO/N10_SUPER.ANM")},
                                  false,
                                  oscilline::Confidence::High);
    auto rig = oscilline::make_character_rig(registry, oscilline::Form::Rabbit);
    CHECK(rig.disc);
    const auto* stored = registry.animation_names(oscilline::Slot::FormBase);
    const auto* clips = registry.animations(oscilline::Slot::FormBase);
    REQUIRE(stored != nullptr);
    REQUIRE(clips != nullptr);
    REQUIRE(stored->size() == clips->size());

    const int idle = clip_index(*stored, "N00_SKIP.ANM");
    const int block = clip_index(*stored, "N01_J.ANM");
    const int pit = clip_index(*stored, "N02_H.ANM");
    const int pair = clip_index(*stored, "N05_JH.ANM");
    const int variant = clip_index(*stored, "N07_JH_F.ANM");
    const int skip_variant = clip_index(*stored, "N08_SKIP_F.ANM");
    REQUIRE(idle >= 0);
    REQUIRE(block >= 0);
    REQUIRE(pit >= 0);
    REQUIRE(pair >= 0);
    REQUIRE(variant >= 0);
    REQUIRE(skip_variant >= 0);
    CHECK(rig.library.idle == idle);
    CHECK(rig.library.action[oscilline::kActionBlock] == block);
    CHECK(rig.library.action[oscilline::kActionPit] == pit);
    CHECK(rig.library.action[oscilline::kActionBlock | oscilline::kActionPit] == pair);
    CHECK(rig.library.action[oscilline::kActionLoop] == clip_index(*stored, "N03_L.ANM"));
    CHECK(rig.library.action[oscilline::kActionWave] == clip_index(*stored, "N04_W.ANM"));
    CHECK(rig.library.action[oscilline::kActionLoop | oscilline::kActionWave] ==
          clip_index(*stored, "N06_LW.ANM"));
    CHECK(rig.library.idle != skip_variant);
    CHECK(rig.library.action[oscilline::kActionBlock | oscilline::kActionPit] != variant);
    CHECK(rig.library.whiff[oscilline::kActionBlock | oscilline::kActionPit] == variant);
    CHECK(rig.library.miss[0] < 0);
    CHECK(rig.library.miss[oscilline::kActionBlock] == clip_index(*stored, "N09_B_MISS.ANM"));

    const auto medium = oscilline::character_library(*stored, oscilline::Confidence::Medium);
    CHECK(medium.action[oscilline::kActionPit] == pit);
    CHECK(medium.action[oscilline::kActionLoop] == clip_index(*stored, "N03_L.ANM"));
    CHECK(medium.action[oscilline::kActionWave] == clip_index(*stored, "N04_W.ANM"));
    CHECK(medium.action[oscilline::kActionLoop | oscilline::kActionWave] ==
          clip_index(*stored, "N06_LW.ANM"));
    CHECK(medium.action[oscilline::kActionBlock | oscilline::kActionPit] == pair);
    CHECK(medium.idle == idle);
    const auto low = oscilline::character_library(*stored, oscilline::Confidence::Low);
    CHECK(low.idle == idle);
    CHECK(low.action[oscilline::kActionBlock | oscilline::kActionPit] == pair);

    oscilline::CharacterClock clock;
    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);
    CHECK_FALSE(clock.oneshot);

    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionBlock, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == block);
    CHECK(clock.oneshot);
    CHECK_FALSE(clock.miss);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.05, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);

    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionPit, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == pit);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.05, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);
    oscilline::character_advance(
        clock,
        rig.library,
        *clips,
        0.0,
        static_cast<std::uint8_t>(oscilline::kActionBlock | oscilline::kActionPit),
        false,
        oscilline::Form::Rabbit);
    CHECK(clock.clip == pair);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.05, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);

    const int loop = clip_index(*stored, "N03_L.ANM");
    const int wave = clip_index(*stored, "N04_W.ANM");
    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionLoop, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == loop);
    CHECK(clock.oneshot);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.05, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionWave, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == wave);
    oscilline::character_advance(
        clock, rig.library, *clips, 0.05, 0, false, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);
    oscilline::character_advance(clock, rig.library, *clips, 0.0, 0, true, oscilline::Form::Rabbit);
    CHECK(clock.clip == idle);
    CHECK_FALSE(clock.miss);

    oscilline::CourseTimeline course;
    course.duration_ms = 5000;
    oscilline::CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 1000;
    event.approach_ms = 1000;
    event.scroll_approach_ms = 1000;
    course.events.push_back(event);
    oscilline::PlayState play;
    const oscilline::ObstacleWindow window = oscilline::obstacle_window(event);
    const std::int64_t when = (window.perfect_open + window.perfect_close) / 2;
    oscilline::play_advance(play, course, window.good_open, when, oscilline::kActionBlock);
    const int score = play.score;
    const oscilline::Form form = play.form;
    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionBlock, false, play.form);
    CHECK(play.score == score);
    CHECK(play.form == form);
    CHECK(play.score == 3);
    CHECK(clock.clip == block);

    event.obstacle = 1;
    course.events.clear();
    course.events.push_back(event);
    oscilline::PlayState pit_play;
    const oscilline::ObstacleWindow pit_window = oscilline::obstacle_window(event);
    const std::int64_t pit_when = (pit_window.perfect_open + pit_window.perfect_close) / 2;
    oscilline::play_advance(
        pit_play, course, pit_window.good_open, pit_when, oscilline::kActionPit);
    const int pit_score = pit_play.score;
    oscilline::character_advance(
        clock, rig.library, *clips, 0.0, oscilline::kActionPit, false, pit_play.form);
    CHECK(pit_play.score == pit_score);
    CHECK(pit_play.score == 3);
    // H is high (checked by eye against the original), so a pit clear plays it.
    CHECK(clock.clip == pit);
    CHECK(clock.clip != block);
}

TEST_CASE("observed clears, whiffs, and wrong presses follow the confidence gate") {
    const std::string report = oscilline::character_clip_report(oscilline::Confidence::High);
    CHECK(report == "J clear high on\n"
                    "J_F whiff high on\n"
                    "L clear high on\n"
                    "L_F whiff high on\n"
                    "W clear high on\n"
                    "W whiff high on\n"
                    "H clear high on\n"
                    "H whiff high on\n"
                    "JL pair high on\n"
                    "JW pair high on\n"
                    "HL pair high on\n"
                    "HW pair high on\n"
                    "LW pair high on\n"
                    "JH pair high on\n"
                    "wrong-press none high on\n");
    CHECK(report.find("high gated") == std::string::npos);
    CHECK(report.find("medium on") == std::string::npos);
    CHECK(report.find("SUPER") == std::string::npos);
    CHECK(report.find("B_MISS") == std::string::npos);
    CHECK(report.find("W_F") == std::string::npos);
    CHECK(report.find("H_F") == std::string::npos);

    const std::vector<std::string> names = {
        "N00_SKIP.ANM",
        "N01_J.ANM",
        "N02_J_F.ANM",
        "N03_L.ANM",
        "N04_L_F.ANM",
        "N05_W.ANM",
        "N06_H.ANM",
        "N07_JL.ANM",
        "N08_JW.ANM",
        "N09_HL.ANM",
        "N10_HW.ANM",
        "N11_LW.ANM",
        "N12_JH.ANM",
    };
    const auto high = oscilline::character_library(names, oscilline::Confidence::High);
    const auto medium = oscilline::character_library(names, oscilline::Confidence::Medium);
    CHECK(high.idle == 0);
    CHECK(high.action[oscilline::kActionBlock] == 1);
    CHECK(high.whiff[oscilline::kActionBlock] == 2);
    CHECK(high.action[oscilline::kActionLoop] == 3);
    CHECK(high.whiff[oscilline::kActionLoop] == 4);
    CHECK(high.action[oscilline::kActionWave] == 5);
    CHECK(high.whiff[oscilline::kActionWave] < 0);
    CHECK(high.action[oscilline::kActionPit] == 6);
    CHECK(high.whiff[oscilline::kActionPit] < 0);
    CHECK(medium.action[oscilline::kActionPit] == 6);
    CHECK(medium.whiff[oscilline::kActionPit] < 0);

    const std::uint8_t pairs[] = {
        static_cast<std::uint8_t>(oscilline::kActionBlock | oscilline::kActionLoop),
        static_cast<std::uint8_t>(oscilline::kActionBlock | oscilline::kActionWave),
        static_cast<std::uint8_t>(oscilline::kActionPit | oscilline::kActionLoop),
        static_cast<std::uint8_t>(oscilline::kActionPit | oscilline::kActionWave),
        static_cast<std::uint8_t>(oscilline::kActionLoop | oscilline::kActionWave),
        static_cast<std::uint8_t>(oscilline::kActionBlock | oscilline::kActionPit),
    };
    const int pair_index[] = {7, 8, 9, 10, 11, 12};
    for (int i = 0; i < 6; ++i) {
        CHECK(high.action[pairs[i]] == pair_index[i]);
        CHECK(medium.action[pairs[i]] == pair_index[i]);
        const auto match = oscilline::match_character_clip(names[static_cast<std::size_t>(7 + i)]);
        CHECK(match.confidence == oscilline::Confidence::High);
        CHECK_FALSE(match.whiff);
    }

    std::vector<oscilline::AnmFile> anims(names.size());
    for (oscilline::AnmFile& clip : anims) {
        clip.unk1 = 10;
        clip.frames.resize(4);
    }
    auto played = [&](const oscilline::CharacterLibrary& library,
                      std::uint8_t actions,
                      oscilline::ClipMoment moment) {
        oscilline::CharacterClock clock;
        oscilline::character_advance(clock, library, anims, 0.0, 0, false, oscilline::Form::Rabbit);
        const bool missed = moment == oscilline::ClipMoment::Wrong;
        oscilline::character_advance(
            clock, library, anims, 0.0, actions, missed, oscilline::Form::Rabbit, moment);
        return clock.clip;
    };
    CHECK(played(high, oscilline::kActionBlock, oscilline::ClipMoment::Clear) == 1);
    CHECK(played(high, oscilline::kActionBlock, oscilline::ClipMoment::Whiff) == 2);
    CHECK(played(high, oscilline::kActionLoop, oscilline::ClipMoment::Clear) == 3);
    CHECK(played(high, oscilline::kActionLoop, oscilline::ClipMoment::Whiff) == 4);
    CHECK(played(high, oscilline::kActionWave, oscilline::ClipMoment::Clear) == 5);
    CHECK(played(high, oscilline::kActionWave, oscilline::ClipMoment::Whiff) == 5);
    CHECK(played(high, oscilline::kActionPit, oscilline::ClipMoment::Clear) == 6);
    CHECK(played(high, oscilline::kActionPit, oscilline::ClipMoment::Whiff) == 6);
    CHECK(played(medium, oscilline::kActionPit, oscilline::ClipMoment::Clear) == 6);
    CHECK(played(medium, oscilline::kActionPit, oscilline::ClipMoment::Whiff) == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(played(high, pairs[i], oscilline::ClipMoment::Clear) == pair_index[i]);
        CHECK(played(medium, pairs[i], oscilline::ClipMoment::Clear) == pair_index[i]);
    }
    CHECK(played(high, oscilline::kActionLoop, oscilline::ClipMoment::Wrong) == 0);
    CHECK(played(high, oscilline::kActionBlock, oscilline::ClipMoment::Wrong) == 0);

    const std::vector<std::string> no_block_whiff = {"N00_SKIP.ANM", "N01_J.ANM", "N05_W.ANM"};
    const auto plain = oscilline::character_library(no_block_whiff, oscilline::Confidence::High);
    std::vector<oscilline::AnmFile> plain_anims(no_block_whiff.size());
    for (oscilline::AnmFile& clip : plain_anims) {
        clip.unk1 = 10;
        clip.frames.resize(4);
    }
    auto played_plain = [&](std::uint8_t actions, oscilline::ClipMoment moment) {
        oscilline::CharacterClock clock;
        oscilline::character_advance(
            clock, plain, plain_anims, 0.0, 0, false, oscilline::Form::Rabbit);
        oscilline::character_advance(
            clock, plain, plain_anims, 0.0, actions, false, oscilline::Form::Rabbit, moment);
        return clock.clip;
    };
    CHECK(plain.whiff[oscilline::kActionBlock] < 0);
    CHECK(plain.whiff[oscilline::kActionWave] < 0);
    CHECK(played_plain(oscilline::kActionBlock, oscilline::ClipMoment::Whiff) == 1);
    CHECK(played_plain(oscilline::kActionWave, oscilline::ClipMoment::Whiff) == 2);

    const std::vector<std::string> low_wave_whiff = {"SKIP.ANM", "W.ANM", "W_F.ANM"};
    const auto wave_gate =
        oscilline::character_library(low_wave_whiff, oscilline::Confidence::High);
    CHECK(wave_gate.whiff[oscilline::kActionWave] == 2);
    CHECK(wave_gate.whiff_stem[oscilline::kActionWave]);
    CHECK(wave_gate.action[oscilline::kActionWave] == 1);
    std::vector<oscilline::AnmFile> wave_anims(low_wave_whiff.size());
    for (oscilline::AnmFile& clip : wave_anims) {
        clip.unk1 = 10;
        clip.frames.resize(4);
    }
    auto played_wave = [&](oscilline::ClipMoment moment) {
        oscilline::CharacterClock clock;
        oscilline::character_advance(
            clock, wave_gate, wave_anims, 0.0, 0, false, oscilline::Form::Frog);
        oscilline::character_advance(clock,
                                     wave_gate,
                                     wave_anims,
                                     0.0,
                                     oscilline::kActionWave,
                                     false,
                                     oscilline::Form::Frog,
                                     moment);
        return clock.clip;
    };
    CHECK(played_wave(oscilline::ClipMoment::Whiff) == 2);
    CHECK(played_wave(oscilline::ClipMoment::Clear) == 1);
    const auto wave_low = oscilline::character_library(low_wave_whiff, oscilline::Confidence::Low);
    CHECK(wave_low.whiff[oscilline::kActionWave] == 2);

    auto step_cue = [](std::uint32_t obstacle, std::uint8_t edges, std::int64_t at) {
        oscilline::CourseTimeline course;
        course.duration_ms = 8000;
        oscilline::CourseEvent event;
        event.obstacle = static_cast<std::uint8_t>(obstacle);
        event.hit_ms = 2000;
        course.events.push_back(event);
        oscilline::PlayState state;
        std::vector<oscilline::PlayHit> hits;
        const oscilline::PlayAdvanceResult step =
            oscilline::play_advance(state, course, 0, at, edges, &hits);
        return std::pair{step, oscilline::action_clip_cue(step, hits, edges)};
    };

    const auto block_clear = step_cue(0, oscilline::kActionBlock, 2000);
    CHECK_FALSE(block_clear.first.whiff);
    CHECK_FALSE(block_clear.first.wrong);
    CHECK(block_clear.second.moment == oscilline::ClipMoment::Clear);
    CHECK(block_clear.second.actions == oscilline::kActionBlock);

    const auto block_whiff = step_cue(0, oscilline::kActionBlock, 400);
    CHECK(block_whiff.first.whiff);
    CHECK_FALSE(block_whiff.first.wrong);
    CHECK(block_whiff.second.moment == oscilline::ClipMoment::Whiff);
    CHECK(block_whiff.second.actions == oscilline::kActionBlock);

    const auto block_wrong = step_cue(0, oscilline::kActionLoop, 2000);
    CHECK_FALSE(block_wrong.first.whiff);
    CHECK(block_wrong.first.wrong);
    CHECK(block_wrong.second.moment == oscilline::ClipMoment::Wrong);
    CHECK(block_wrong.second.actions == oscilline::kActionLoop);
    CHECK(played(high, block_wrong.second.actions, block_wrong.second.moment) == 0);

    const auto loop_clear = step_cue(2, oscilline::kActionLoop, 2000);
    CHECK(loop_clear.second.moment == oscilline::ClipMoment::Clear);
    CHECK(loop_clear.second.actions == oscilline::kActionLoop);
    const auto loop_whiff = step_cue(2, oscilline::kActionLoop, 400);
    CHECK(loop_whiff.second.moment == oscilline::ClipMoment::Whiff);
    CHECK(loop_whiff.second.actions == oscilline::kActionLoop);

    const auto wave_clear = step_cue(3, oscilline::kActionWave, 2000);
    CHECK(wave_clear.second.moment == oscilline::ClipMoment::Clear);
    CHECK(wave_clear.second.actions == oscilline::kActionWave);
    const auto wave_whiff = step_cue(3, oscilline::kActionWave, 400);
    CHECK(wave_whiff.second.moment == oscilline::ClipMoment::Whiff);
    CHECK(wave_whiff.second.actions == oscilline::kActionWave);
    CHECK(played(high, wave_whiff.second.actions, wave_whiff.second.moment) == 5);

    const auto pit_clear = step_cue(1, oscilline::kActionPit, 2000);
    CHECK(pit_clear.second.moment == oscilline::ClipMoment::Clear);
    CHECK(pit_clear.second.actions == oscilline::kActionPit);
    CHECK(played(high, pit_clear.second.actions, pit_clear.second.moment) == 6);
    CHECK(played(medium, pit_clear.second.actions, pit_clear.second.moment) == 6);
    const auto pit_whiff = step_cue(1, oscilline::kActionPit, 400);
    CHECK(pit_whiff.second.moment == oscilline::ClipMoment::Whiff);
    CHECK(played(high, pit_whiff.second.actions, pit_whiff.second.moment) == 6);
    CHECK(played(medium, pit_whiff.second.actions, pit_whiff.second.moment) == 6);

    const std::uint32_t pair_obstacles[] = {5, 6, 7, 8, 9, 4};
    for (int i = 0; i < 6; ++i) {
        const auto cleared = step_cue(pair_obstacles[i], pairs[i], 2000);
        CHECK(cleared.second.moment == oscilline::ClipMoment::Clear);
        CHECK(cleared.second.actions == pairs[i]);
        CHECK(played(high, cleared.second.actions, cleared.second.moment) == pair_index[i]);
        CHECK(played(medium, cleared.second.actions, cleared.second.moment) == pair_index[i]);
    }

    CHECK_FALSE(oscilline::match_character_clip("N08_SUPER.ANM").matched);
    const auto miss = oscilline::match_character_clip("B_MISS.ANM");
    CHECK(miss.kind == oscilline::ClipKind::Miss);
    CHECK(miss.confidence == oscilline::Confidence::High);
}

TEST_CASE("placeholder figures keep their golden segments") {
    std::vector<oscilline::Segment> rabbit;
    oscilline::placeholder_figure(rabbit, oscilline::Form::Rabbit, 80.f, 200.f);
    REQUIRE(rabbit.size() == 13);
    expect_segment(rabbit[0], 73.f, 128.f, 87.f, 128.f);
    expect_segment(rabbit[1], 87.f, 128.f, 87.f, 142.f);
    expect_segment(rabbit[2], 87.f, 142.f, 73.f, 142.f);
    expect_segment(rabbit[3], 73.f, 142.f, 73.f, 128.f);
    expect_segment(rabbit[4], 75.f, 128.f, 70.f, 112.f);
    expect_segment(rabbit[5], 70.f, 112.f, 79.f, 126.f);
    expect_segment(rabbit[6], 85.f, 128.f, 90.f, 112.f);
    expect_segment(rabbit[7], 90.f, 112.f, 81.f, 126.f);
    expect_segment(rabbit[8], 80.f, 142.f, 80.f, 190.f);
    expect_segment(rabbit[9], 80.f, 158.f, 64.f, 176.f);
    expect_segment(rabbit[10], 80.f, 158.f, 96.f, 176.f);
    expect_segment(rabbit[11], 80.f, 190.f, 68.f, 198.f);
    expect_segment(rabbit[12], 80.f, 190.f, 92.f, 198.f);
    CHECK(rabbit[12].y1 == doctest::Approx(200.f - oscilline::kFigureRibbonClearancePx));
    for (const oscilline::Segment& segment : rabbit) {
        CHECK(segment.color.r == doctest::Approx(1.f));
    }

    std::vector<oscilline::Segment> shifted;
    oscilline::placeholder_figure(shifted, oscilline::Form::Rabbit, 100.f, 220.f);
    REQUIRE(shifted.size() == rabbit.size());
    for (std::size_t i = 0; i < rabbit.size(); ++i) {
        CHECK(shifted[i].x0 == doctest::Approx(rabbit[i].x0 + 20.f));
        CHECK(shifted[i].y0 == doctest::Approx(rabbit[i].y0 + 20.f));
        CHECK(shifted[i].x1 == doctest::Approx(rabbit[i].x1 + 20.f));
        CHECK(shifted[i].y1 == doctest::Approx(rabbit[i].y1 + 20.f));
    }

    std::vector<oscilline::Segment> frog;
    oscilline::placeholder_figure(frog, oscilline::Form::Frog, 10.f, 50.f);
    REQUIRE(frog.size() == 12);
    expect_segment(frog[0], -6.f, 26.f, 26.f, 26.f);
    expect_segment(frog[1], 26.f, 26.f, 22.f, 40.f);
    expect_segment(frog[2], 22.f, 40.f, -2.f, 40.f);
    expect_segment(frog[3], -2.f, 40.f, -6.f, 26.f);
    expect_segment(frog[4], 2.f, 24.f, 6.f, 18.f);
    expect_segment(frog[5], 6.f, 18.f, 10.f, 24.f);
    expect_segment(frog[6], 18.f, 24.f, 14.f, 18.f);
    expect_segment(frog[7], 14.f, 18.f, 10.f, 24.f);
    expect_segment(frog[8], -4.f, 32.f, -16.f, 46.f);
    expect_segment(frog[9], -16.f, 46.f, -4.f, 48.f);
    expect_segment(frog[10], 24.f, 32.f, 36.f, 46.f);
    expect_segment(frog[11], 36.f, 46.f, 24.f, 48.f);

    std::vector<oscilline::Segment> worm;
    oscilline::placeholder_figure(worm, oscilline::Form::Worm, 10.f, 50.f);
    REQUIRE(worm.size() == 3);
    expect_segment(worm[0], -4.f, 46.f, 4.f, 40.f);
    expect_segment(worm[1], 4.f, 40.f, 14.f, 46.f);
    expect_segment(worm[2], 14.f, 46.f, 24.f, 41.f);

    std::vector<oscilline::Segment> out;
    oscilline::placeholder_figure(out, oscilline::Form::Out, 10.f, 50.f);
    CHECK(out.empty());

    std::vector<oscilline::Segment> super;
    oscilline::placeholder_figure(super, oscilline::Form::Super, 80.f, 200.f);
    REQUIRE(super.size() > rabbit.size());
    CHECK(super[0].x0 == doctest::Approx(rabbit[0].x0));
    CHECK(super[0].y0 == doctest::Approx(rabbit[0].y0));
}

TEST_CASE("the clips report follows the confidence floor and the form's stems") {
    const std::string high = oscilline::character_clip_report(oscilline::Confidence::High);
    const std::string medium = oscilline::character_clip_report(oscilline::Confidence::Medium);
    const std::string low = oscilline::character_clip_report(oscilline::Confidence::Low);
    CHECK(high.find("H clear high on\n") != std::string::npos);
    CHECK(high.find("H whiff high on\n") != std::string::npos);
    CHECK(high.find("JH pair high on\n") != std::string::npos);
    CHECK(high.find("LW pair high on\n") != std::string::npos);
    CHECK(high.find("JL pair high on\n") != std::string::npos);
    CHECK(medium.find("gated") == std::string::npos);
    CHECK(medium.find("H clear high on\n") != std::string::npos);
    CHECK(medium.find("JH pair high on\n") != std::string::npos);
    CHECK(medium.find("JL pair high on\n") != std::string::npos);
    CHECK(medium.find("J clear high on\n") != std::string::npos);
    CHECK(low.find("gated") == std::string::npos);

    const std::vector<std::string> rabbit = {"CHARA/PEELOO/N00_SKIP.ANM",
                                             "CHARA/PEELOO/N01_J.ANM",
                                             "CHARA/PEELOO/N01_J_F.ANM",
                                             "CHARA/PEELOO/N02_H.ANM",
                                             "CHARA/PEELOO/N04_W.ANM",
                                             "CHARA/PEELOO/N05_JH.ANM"};
    const std::string rabbit_high =
        oscilline::character_clip_resolve("rabbit", rabbit, oscilline::Confidence::High);
    CHECK(rabbit_high.find("rabbit J clear N01_J.ANM\n") != std::string::npos);
    CHECK(rabbit_high.find("rabbit J_F whiff N01_J_F.ANM\n") != std::string::npos);
    CHECK(rabbit_high.find("rabbit W whiff N04_W.ANM\n") != std::string::npos);
    // The rabbit has no H_F, so its pit whiff plays H at H's confidence.
    CHECK(rabbit_high.find("rabbit H clear N02_H.ANM\n") != std::string::npos);
    CHECK(rabbit_high.find("rabbit H whiff N02_H.ANM\n") != std::string::npos);
    CHECK(rabbit_high.find("rabbit JH pair N05_JH.ANM\n") != std::string::npos);
    CHECK(rabbit_high.find("rabbit wrong-press none none\n") != std::string::npos);
    const std::string rabbit_medium =
        oscilline::character_clip_resolve("rabbit", rabbit, oscilline::Confidence::Medium);
    CHECK(rabbit_medium.find("rabbit H clear N02_H.ANM\n") != std::string::npos);
    CHECK(rabbit_medium.find("rabbit H whiff N02_H.ANM\n") != std::string::npos);
    CHECK(rabbit_medium.find("rabbit JH pair N05_JH.ANM\n") != std::string::npos);
    CHECK(rabbit_medium.find("CHARA/") == std::string::npos);

    // A worm-like form: W_F and H_F are high, so those whiffs play the _F clip.
    const std::vector<std::string> worm = {
        "N00_SKIP.ANM", "N02_H.ANM", "N02_H_F.ANM", "N04_W.ANM", "N04_W_F.ANM"};
    const std::string worm_high =
        oscilline::character_clip_resolve("worm", worm, oscilline::Confidence::High);
    CHECK(worm_high.find("worm W clear N04_W.ANM\n") != std::string::npos);
    CHECK(worm_high.find("worm W whiff N04_W_F.ANM\n") != std::string::npos);
    CHECK(worm_high.find("worm H clear N02_H.ANM\n") != std::string::npos);
    CHECK(worm_high.find("worm H whiff N02_H_F.ANM\n") != std::string::npos);
    const std::string worm_medium =
        oscilline::character_clip_resolve("worm", worm, oscilline::Confidence::Medium);
    CHECK(worm_medium.find("worm H clear N02_H.ANM\n") != std::string::npos);
    CHECK(worm_medium.find("worm H whiff N02_H_F.ANM\n") != std::string::npos);
    const std::string worm_low =
        oscilline::character_clip_resolve("worm", worm, oscilline::Confidence::Low);
    CHECK(worm_low.find("worm W whiff N04_W_F.ANM\n") != std::string::npos);
    CHECK(worm_low.find("worm H whiff N02_H_F.ANM\n") != std::string::npos);
}

TEST_CASE("the super clip library uses the rabbit stems and confidence") {
    const std::vector<std::string> stems = {
        "CHARA/SUPER/N00_SKIP.ANM",
        "CHARA/SUPER/N01_J.ANM",
        "CHARA/SUPER/N01_J_F.ANM",
        "CHARA/SUPER/N02_H.ANM",
        "CHARA/SUPER/N03_L.ANM",
        "CHARA/SUPER/N03_L_F.ANM",
        "CHARA/SUPER/N04_W.ANM",
        "CHARA/SUPER/N05_JH.ANM",
        "CHARA/SUPER/N06_JL.ANM",
        "CHARA/SUPER/N07_JW.ANM",
        "CHARA/SUPER/N08_HL.ANM",
        "CHARA/SUPER/N09_HW.ANM",
        "CHARA/SUPER/N10_LW.ANM",
    };
    const auto high = oscilline::character_library(stems, oscilline::Confidence::High);
    CHECK(high.usable);
    CHECK(high.idle == 0);
    CHECK(high.action[oscilline::kActionBlock] == 1);
    CHECK(high.whiff[oscilline::kActionBlock] == 2);
    CHECK(high.action[oscilline::kActionPit] == 3);
    CHECK(high.action[oscilline::kActionLoop] == 4);
    CHECK(high.whiff[oscilline::kActionLoop] == 5);
    CHECK(high.action[oscilline::kActionWave] == 6);
    CHECK(high.action[oscilline::kActionBlock | oscilline::kActionPit] == 7);
    CHECK(high.action[oscilline::kActionBlock | oscilline::kActionLoop] == 8);
    CHECK(high.action[oscilline::kActionBlock | oscilline::kActionWave] == 9);
    CHECK(high.action[oscilline::kActionPit | oscilline::kActionLoop] == 10);
    CHECK(high.action[oscilline::kActionPit | oscilline::kActionWave] == 11);
    CHECK(high.action[oscilline::kActionLoop | oscilline::kActionWave] == 12);
    CHECK(high.whiff[oscilline::kActionPit] < 0);

    std::vector<std::string> rabbit = stems;
    for (std::string& name : rabbit) {
        const auto slash = name.rfind('/');
        name.replace(0, slash, "CHARA/PEELOO");
    }
    const auto rabbit_high = oscilline::character_library(rabbit, oscilline::Confidence::High);
    CHECK(rabbit_high.idle == high.idle);
    CHECK(rabbit_high.action[oscilline::kActionBlock] == high.action[oscilline::kActionBlock]);
    CHECK(rabbit_high.action[oscilline::kActionPit] == high.action[oscilline::kActionPit]);
    CHECK(rabbit_high.whiff[oscilline::kActionBlock] == high.whiff[oscilline::kActionBlock]);
    CHECK(rabbit_high.action[oscilline::kActionBlock | oscilline::kActionPit] ==
          high.action[oscilline::kActionBlock | oscilline::kActionPit]);

    const std::string report =
        oscilline::character_clip_resolve("super", stems, oscilline::Confidence::High);
    CHECK(report.find("super J clear N01_J.ANM\n") != std::string::npos);
    CHECK(report.find("super J_F whiff N01_J_F.ANM\n") != std::string::npos);
    CHECK(report.find("super H clear N02_H.ANM\n") != std::string::npos);
    CHECK(report.find("super H whiff N02_H.ANM\n") != std::string::npos);
    CHECK(report.find("super L clear N03_L.ANM\n") != std::string::npos);
    CHECK(report.find("super L_F whiff N03_L_F.ANM\n") != std::string::npos);
    CHECK(report.find("super W clear N04_W.ANM\n") != std::string::npos);
    CHECK(report.find("super JH pair N05_JH.ANM\n") != std::string::npos);
    CHECK(report.find("super JL pair N06_JL.ANM\n") != std::string::npos);
    CHECK(report.find("super JW pair N07_JW.ANM\n") != std::string::npos);
    CHECK(report.find("super HL pair N08_HL.ANM\n") != std::string::npos);
    CHECK(report.find("super HW pair N09_HW.ANM\n") != std::string::npos);
    CHECK(report.find("super LW pair N10_LW.ANM\n") != std::string::npos);
    CHECK(report.find("SUPER") == std::string::npos);

    oscilline::CharacterClock clock;
    oscilline::apply_super_transform_clip(clock, -1, static_cast<int>(stems.size()));
    CHECK(clock.clip < 0);
    oscilline::apply_super_transform_clip(clock, 1, static_cast<int>(stems.size()));
    CHECK(clock.clip == 1);
    CHECK(clock.oneshot);
    clock.seconds = 0.4;
    oscilline::apply_super_transform_clip(clock, 1, static_cast<int>(stems.size()));
    CHECK(clock.seconds == doctest::Approx(0.4));
}

namespace {

oscilline::TmdPrimitive eye_line(std::uint16_t a, std::uint16_t b) {
    oscilline::TmdPrimitive primitive;
    primitive.kind = oscilline::PrimitiveKind::Line;
    primitive.vertex_indices = {a, b};
    primitive.color = 0x00FFFFFFu;
    return primitive;
}

oscilline::TmdPrimitive
eye_poly(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint32_t color) {
    oscilline::TmdPrimitive primitive;
    primitive.kind = oscilline::PrimitiveKind::Polygon;
    primitive.vertex_indices = {a, b, c};
    primitive.color = color;
    return primitive;
}

oscilline::TmdModel eyed_figure() {
    oscilline::TmdModel model;
    oscilline::TmdObject body;
    body.vertices = {{0, 0, 0}, {0, -80, 0}};
    body.primitives.push_back(eye_line(0, 1));
    oscilline::TmdObject left;
    left.vertices = {{-10, -72, 1}, {-6, -72, 1}, {-8, -66, 1}};
    left.primitives.push_back(eye_poly(0, 1, 2, 0x000000FFu));
    oscilline::TmdObject right;
    right.vertices = {{6, -72, 1}, {10, -72, 1}, {8, -66, 1}};
    right.primitives.push_back(eye_poly(0, 1, 2, 0x0000FF00u));
    model.objects.push_back(std::move(body));
    model.objects.push_back(std::move(left));
    model.objects.push_back(std::move(right));
    return model;
}

oscilline::AnmFile keyed_eyes() {
    oscilline::AnmFile clip;
    clip.unk1 = 30;
    clip.frame_count = 1;
    clip.frames.resize(1);
    for (std::uint8_t index = 0; index < 3; ++index) {
        oscilline::AnmKeyframe key;
        key.object_index = index;
        clip.frames[0].keys.push_back(key);
    }
    return clip;
}

int fills_of(const std::vector<oscilline::FilledTriangle>& fills, bool red) {
    int count = 0;
    for (const oscilline::FilledTriangle& fill : fills) {
        const bool is_red = fill.color.r > 0.9f && fill.color.g < 0.1f;
        const bool is_green = fill.color.g > 0.9f && fill.color.r < 0.1f;
        if (red ? is_red : is_green) {
            ++count;
        }
    }
    return count;
}

} // namespace

namespace {

// A play-model figure: the body, two eye objects marked by a pure black fill
// (the packet's top byte is the 0x21 primitive code), and a white outline on
// each eye. Left is model -X.
oscilline::TmdModel black_eyed_figure(std::int16_t left_z = 1) {
    oscilline::TmdModel model;
    oscilline::TmdObject body;
    body.vertices = {{0, 0, 0}, {0, -80, 0}};
    body.primitives.push_back(eye_line(0, 1));
    oscilline::TmdObject left;
    left.vertices = {{-10, -72, left_z}, {-6, -72, left_z}, {-8, -66, left_z}};
    left.primitives.push_back(eye_poly(0, 1, 2, 0x21000000u));
    left.primitives.push_back(eye_line(0, 1));
    oscilline::TmdObject right;
    right.vertices = {{6, -72, 1}, {10, -72, 1}, {8, -66, 1}};
    right.primitives.push_back(eye_poly(0, 1, 2, 0x21000000u));
    right.primitives.push_back(eye_line(0, 1));
    model.objects.push_back(std::move(body));
    model.objects.push_back(std::move(left));
    model.objects.push_back(std::move(right));
    return model;
}

// How many eye fills are drawn, and their mean screen x (NaN when none).
std::pair<int, float> eye_fills(const std::vector<oscilline::FilledTriangle>& fills) {
    int count = 0;
    float x = 0.f;
    for (const oscilline::FilledTriangle& fill : fills) {
        if (fill.color.r < 0.05f && fill.color.g < 0.05f && fill.color.b < 0.05f) {
            ++count;
            x += (fill.x0 + fill.x1 + fill.x2) / 3.f;
        }
    }
    return {count, count > 0 ? x / static_cast<float>(count) : std::nanf("")};
}

std::vector<oscilline::FilledTriangle> paint_eyes(const oscilline::TmdModel& model,
                                                  const oscilline::AncSample* camera) {
    static const oscilline::AnmFile clip = keyed_eyes();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement.valid = true;
    pose.placement.scale = 1.f;
    std::vector<oscilline::Segment> lines;
    std::vector<oscilline::FilledTriangle> fills;
    CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, &fills, camera));
    return fills;
}

oscilline::AncSample camera_at(float degrees) {
    const float yaw = degrees * std::numbers::pi_v<float> / 180.f;
    oscilline::AncSample camera;
    camera.eye_x = std::sin(yaw) * 1000.f;
    camera.eye_z = std::cos(yaw) * 1000.f;
    camera.fov = 500.f;
    return camera;
}

} // namespace

TEST_CASE("play-model eyes: both face on, the far one past 45 degrees, none past 150") {
    const oscilline::TmdModel model = black_eyed_figure();
    const oscilline::AncSample face = camera_at(0.f);
    CHECK(eye_fills(paint_eyes(model, &face)).first == 2);
    const oscilline::AncSample turned = camera_at(30.f);
    CHECK(eye_fills(paint_eyes(model, &turned)).first == 2);

    // From her right (+X), the left eye is the far one; the right eye stays.
    const oscilline::AncSample right_side = camera_at(68.f);
    CHECK(eye_fills(paint_eyes(model, &right_side)).first == 1);
    const oscilline::AncSample left_side = camera_at(-68.f);
    CHECK(eye_fills(paint_eyes(model, &left_side)).first == 1);
    // The two sides keep opposite eyes.
    CHECK(eye_fills(paint_eyes(model, &right_side)).second !=
          doctest::Approx(eye_fills(paint_eyes(model, &left_side)).second));

    // B01 sits about 118 degrees off her face: still one eye.
    const oscilline::AncSample behind = camera_at(118.f);
    CHECK(eye_fills(paint_eyes(model, &behind)).first == 1);
    const oscilline::AncSample away = camera_at(155.f);
    CHECK(eye_fills(paint_eyes(model, &away)).first == 0);
    const oscilline::AncSample back = camera_at(180.f);
    CHECK(eye_fills(paint_eyes(model, &back)).first == 0);

    // The side view, with no camera, is a profile: one eye.
    CHECK(eye_fills(paint_eyes(model, nullptr)).first == 1);
}

TEST_CASE("play-model eyes: the hidden eye is the one farther away in the pose") {
    // From 60 degrees to her right, the right eye is nearer on a plain figure.
    const oscilline::AncSample camera = camera_at(60.f);
    const oscilline::TmdModel plain = black_eyed_figure();
    const auto plain_kept = eye_fills(paint_eyes(plain, &camera));
    REQUIRE(plain_kept.first == 1);
    // Push the left eye well toward the face: now it is the nearer one, so the
    // right eye hides instead and the kept fill moves.
    const oscilline::TmdModel forward = black_eyed_figure(60);
    const auto forward_kept = eye_fills(paint_eyes(forward, &camera));
    REQUIRE(forward_kept.first == 1);
    CHECK(forward_kept.second != doctest::Approx(plain_kept.second).epsilon(0.01));
}

TEST_CASE("eye visibility helpers remain while gameplay keeps both eyes visible") {
    using oscilline::eye_visibility_for_yaw;
    const float pi = std::numbers::pi_v<float>;
    const auto face = eye_visibility_for_yaw(0.f);
    CHECK(face.left);
    CHECK(face.right);
    const auto boundary = eye_visibility_for_yaw(oscilline::kEyeBothMaxRadians);
    CHECK(boundary.left);
    CHECK(boundary.right);
    const auto off = eye_visibility_for_yaw(oscilline::kEyeBothMaxRadians + 0.02f);
    CHECK_FALSE(off.left);
    CHECK(off.right);
    const auto side = eye_visibility_for_yaw(oscilline::kEyeSideYawRadians);
    CHECK_FALSE(side.left);
    CHECK(side.right);
    const auto other = eye_visibility_for_yaw(-oscilline::kEyeSideYawRadians);
    CHECK(other.left);
    CHECK_FALSE(other.right);
    const auto past_side = eye_visibility_for_yaw(120.f * pi / 180.f);
    CHECK_FALSE(past_side.left);
    CHECK(past_side.right);
    const auto at_limit = eye_visibility_for_yaw(oscilline::kEyeAwayEnterRadians - 0.01f);
    CHECK_FALSE(at_limit.left);
    CHECK(at_limit.right);
    const auto away = eye_visibility_for_yaw(oscilline::kEyeAwayEnterRadians + 0.02f);
    CHECK_FALSE(away.left);
    CHECK_FALSE(away.right);
    const auto behind = eye_visibility_for_yaw(pi);
    CHECK_FALSE(behind.left);
    CHECK_FALSE(behind.right);

    CHECK(oscilline::figure_yaw_to_camera(0.f, 400.f) == doctest::Approx(0.f));
    CHECK(oscilline::figure_yaw_to_camera(400.f, 0.f) == doctest::Approx(pi * 0.5f));
    CHECK(std::fabs(oscilline::figure_yaw_to_camera(0.f, -400.f)) == doctest::Approx(pi));

    const oscilline::TmdModel model = eyed_figure();
    const std::vector<oscilline::FigureEye> eyes = oscilline::classify_figure_eyes(model);
    REQUIRE(eyes.size() == 3);
    CHECK(eyes[0] == oscilline::FigureEye::None);
    CHECK(eyes[1] == oscilline::FigureEye::Left);
    CHECK(eyes[2] == oscilline::FigureEye::Right);

    // The outline shares the eye object. Hiding the fill has to hide that line too.
    oscilline::TmdModel mixed = model;
    mixed.objects[1].primitives.push_back(eye_line(0, 1));
    mixed.objects[2].primitives.push_back(eye_line(0, 1));
    const std::vector<oscilline::FigureEye> with_outline = oscilline::classify_figure_eyes(mixed);
    CHECK(with_outline[1] == oscilline::FigureEye::Left);
    CHECK(with_outline[2] == oscilline::FigureEye::Right);

    const oscilline::AnmFile clip = keyed_eyes();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement.valid = true;
    pose.placement.scale = 1.f;
    const auto paint = [&](const oscilline::AncSample* camera) {
        std::vector<oscilline::Segment> lines;
        std::vector<oscilline::FilledTriangle> fills;
        CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, &fills, camera));
        CHECK(lines.size() == 1);
        return fills;
    };
    const std::vector<oscilline::FilledTriangle> profile = paint(nullptr);
    CHECK(fills_of(profile, true) == 1);
    CHECK(fills_of(profile, false) == 1);

    oscilline::AncSample face_cam;
    face_cam.eye_z = 1000.f;
    face_cam.fov = 500.f;
    const std::vector<oscilline::FilledTriangle> both = paint(&face_cam);
    CHECK(fills_of(both, true) == 1);
    CHECK(fills_of(both, false) == 1);

    oscilline::AncSample side_cam;
    side_cam.eye_x = 1000.f;
    side_cam.fov = 500.f;
    const std::vector<oscilline::FilledTriangle> one = paint(&side_cam);
    CHECK(fills_of(one, true) == 1);
    CHECK(fills_of(one, false) == 1);

    const float yaw60 = 60.f * pi / 180.f;
    oscilline::AncSample angled;
    angled.eye_x = std::sin(yaw60) * 1000.f;
    angled.eye_z = std::cos(yaw60) * 1000.f;
    angled.fov = 500.f;
    const std::vector<oscilline::FilledTriangle> far = paint(&angled);
    CHECK(fills_of(far, true) == 1);
    CHECK(fills_of(far, false) == 1);

    oscilline::AncSample other_side;
    other_side.eye_x = -std::sin(yaw60) * 1000.f;
    other_side.eye_z = std::cos(yaw60) * 1000.f;
    other_side.fov = 500.f;
    const std::vector<oscilline::FilledTriangle> near_left = paint(&other_side);
    CHECK(fills_of(near_left, true) == 1);
    CHECK(fills_of(near_left, false) == 1);

    oscilline::AncSample back_cam;
    back_cam.eye_z = -1000.f;
    back_cam.fov = 500.f;
    const std::vector<oscilline::FilledTriangle> none = paint(&back_cam);
    CHECK(fills_of(none, true) == 1);
    CHECK(fills_of(none, false) == 1);

    oscilline::ScreenRect rect;
    rect.left = 0.f;
    rect.top = 0.f;
    rect.right = 100.f;
    rect.bottom = 100.f;
    const std::vector<oscilline::Segment> full =
        oscilline::menu_model_frame(model, nullptr, {}, rect, false);
    const std::vector<oscilline::Segment> profile_menu =
        oscilline::menu_model_frame(model, nullptr, {}, rect, true);
    CHECK(full.size() == 7);
    CHECK(profile_menu.size() == 4);
}

TEST_CASE("gameplay painting keeps every part of both eyes visible") {
    oscilline::TmdModel model = eyed_figure();
    // Outline of the left eye, stored on the body object. The fill is object 1.
    model.objects[0].vertices.push_back({-10, -72, 1});
    model.objects[0].vertices.push_back({-6, -72, 1});
    model.objects[0].vertices.push_back({-8, -66, 1});
    const auto outline = static_cast<std::uint16_t>(model.objects[0].vertices.size() - 3);
    oscilline::TmdPrimitive eye_outline =
        eye_line(outline, static_cast<std::uint16_t>(outline + 1));
    eye_outline.color = 0x00FF0000u;
    model.objects[0].primitives.push_back(eye_outline);
    model.objects[0].primitives.push_back(
        eye_line(static_cast<std::uint16_t>(outline + 1), static_cast<std::uint16_t>(outline + 2)));
    model.objects[0].primitives.push_back(
        eye_line(static_cast<std::uint16_t>(outline + 2), outline));
    // A second left-eye polygon, posed onto the fill from the origin.
    oscilline::TmdObject pupil;
    pupil.vertices = {{0, 0, 0}, {2, 0, 0}, {1, 3, 0}};
    pupil.primitives.push_back(eye_poly(0, 1, 2, 0x000000FFu));
    model.objects.push_back(std::move(pupil));

    oscilline::AnmFile clip;
    clip.unk1 = 30;
    clip.frame_count = 1;
    clip.frames.resize(1);
    for (std::uint8_t index = 0; index < 4; ++index) {
        oscilline::AnmKeyframe key;
        key.object_index = index;
        if (index == 3) {
            key.has_position = true;
            key.position_x = -8;
            key.position_y = -70;
        }
        clip.frames[0].keys.push_back(key);
    }
    const std::vector<oscilline::Pose> posed =
        oscilline::poses_for_frame(clip, model.objects.size(), 0, 0.f, false);
    const std::vector<oscilline::FigureEye> bind = oscilline::classify_figure_eyes(model);
    const std::vector<oscilline::FigureEye> placed = oscilline::classify_figure_eyes(model, posed);
    REQUIRE(bind.size() == 4);
    REQUIRE(placed.size() == 4);
    CHECK(bind[3] == oscilline::FigureEye::None);
    CHECK(placed[1] == oscilline::FigureEye::Left);
    CHECK(placed[2] == oscilline::FigureEye::Right);
    CHECK(placed[3] == oscilline::FigureEye::Left);

    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement.valid = true;
    pose.placement.scale = 1.f;
    std::vector<oscilline::Segment> lines;
    std::vector<oscilline::FilledTriangle> fills;
    CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, &fills, nullptr));
    CHECK(fills_of(fills, true) == 2);
    CHECK(fills_of(fills, false) == 1);
    int blue = 0;
    for (const oscilline::Segment& line : lines) {
        if (line.color.b > 0.9f && line.color.r < 0.1f && line.color.g < 0.1f) {
            ++blue;
        }
    }
    CHECK(blue == 1);
    CHECK(lines.size() == 4);

    oscilline::AncSample face_cam;
    face_cam.eye_z = 1000.f;
    face_cam.fov = 500.f;
    lines.clear();
    fills.clear();
    CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, &fills, &face_cam));
    CHECK(fills_of(fills, true) == 2);
    CHECK(fills_of(fills, false) == 1);
    CHECK(lines.size() == 4);
}

TEST_CASE("projected figure strokes crossing the near plane are clipped") {
    oscilline::TmdModel model;
    oscilline::TmdObject eye;
    eye.vertices = {{0, 0, 0}, {20, 0, 20}};
    eye.primitives.push_back(eye_line(0, 1));
    model.objects.push_back(std::move(eye));

    const oscilline::AnmFile clip = keyed_eyes();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement.valid = true;
    pose.placement.scale = 1.f;

    oscilline::AncSample camera;
    camera.eye_z = 10.f;
    camera.fov = 500.f;
    std::vector<oscilline::Segment> lines;
    CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, nullptr, &camera));
    REQUIRE(lines.size() == 1);
    CHECK(std::isfinite(lines[0].x0));
    CHECK(std::isfinite(lines[0].y0));
    CHECK(std::isfinite(lines[0].x1));
    CHECK(std::isfinite(lines[0].y1));
    CHECK((lines[0].x0 != lines[0].x1 || lines[0].y0 != lines[0].y1));
}

TEST_CASE("eye culling hysteresis is experimental and gameplay keeps both eyes visible") {
    const float deg = std::numbers::pi_v<float> / 180.f;
    const auto yaw_of = [&](float degrees) { return degrees * deg; };
    CHECK(oscilline::kEyeHideFromCameraDegrees == doctest::Approx(45.f));
    CHECK(oscilline::kEyeShowFromCameraDegrees == doctest::Approx(40.f));
    CHECK(oscilline::kEyeBothMaxRadians ==
          doctest::Approx(oscilline::kEyeHideFromCameraDegrees * deg));
    CHECK(oscilline::kEyeBothShowRadians ==
          doctest::Approx(oscilline::kEyeShowFromCameraDegrees * deg));
    CHECK(oscilline::kEyeAwaySlackDegrees == doctest::Approx(4.f));
    CHECK(oscilline::kEyeAwayEnterRadians == doctest::Approx(oscilline::kEyeHideBothDegrees * deg));
    CHECK(
        oscilline::kEyeAwayRadians ==
        doctest::Approx((oscilline::kEyeHideBothDegrees - oscilline::kEyeAwaySlackDegrees) * deg));

    const auto check_far_eye = [&](float sign) {
        oscilline::EyeCullState state;
        const auto at = [&](float degrees) {
            return oscilline::eye_visibility_for_yaw(sign * yaw_of(degrees), state);
        };
        const auto far_shown = [&](const oscilline::EyeVisibility& visibility) {
            return sign > 0.f ? visibility.left : visibility.right;
        };
        const auto near_shown = [&](const oscilline::EyeVisibility& visibility) {
            return sign > 0.f ? visibility.right : visibility.left;
        };

        oscilline::EyeCullState boundary;
        const auto on_boundary =
            oscilline::eye_visibility_for_yaw(sign * oscilline::kEyeBothMaxRadians, boundary);
        CHECK(far_shown(on_boundary));
        CHECK(near_shown(on_boundary));

        for (int degrees = 0; degrees <= 44; ++degrees) {
            const auto visibility = at(static_cast<float>(degrees));
            CHECK(far_shown(visibility));
            CHECK(near_shown(visibility));
        }
        const auto at_hide =
            oscilline::eye_visibility_for_yaw(sign * oscilline::kEyeBothMaxRadians, state);
        CHECK(far_shown(at_hide));
        CHECK(near_shown(at_hide));

        for (int degrees = 46; degrees <= 60; ++degrees) {
            const auto visibility = at(static_cast<float>(degrees));
            CHECK_FALSE(far_shown(visibility));
            CHECK(near_shown(visibility));
        }
        // Back down across 45°. The far eye stays hidden through the band.
        for (int degrees = 50; degrees >= 41; --degrees) {
            const auto visibility = at(static_cast<float>(degrees));
            CHECK_FALSE(far_shown(visibility));
            CHECK(near_shown(visibility));
        }
        const auto at_show =
            oscilline::eye_visibility_for_yaw(sign * oscilline::kEyeBothShowRadians, state);
        CHECK_FALSE(far_shown(at_show));
        CHECK(near_shown(at_show));

        const float hidden_noise[] = {44.9f, 43.2f, 41.1f, 40.2f, 45.f, 42.5f, 44.1f, 40.6f};
        for (float degrees : hidden_noise) {
            for (float noise : {-1.4f, -0.6f, 0.f, 0.5f, 1.3f}) {
                const float sample = std::clamp(degrees + noise, 40.15f, 45.f);
                const auto visibility = at(sample);
                CHECK_FALSE(far_shown(visibility));
                CHECK(near_shown(visibility));
            }
        }

        CHECK(far_shown(at(39.f)));
        CHECK(near_shown(at(39.f)));
        // Climb back through the band. Small noise must not hide the eye again.
        const float shown_noise[] = {39.5f, 40.2f, 42.5f, 44.f, 41.4f, 43.7f, 44.8f};
        for (float degrees : shown_noise) {
            for (float noise : {-1.4f, -0.3f, 0.4f, 1.2f}) {
                const float sample = std::clamp(degrees + noise, 39.f, 44.85f);
                const auto visibility = at(sample);
                CHECK(far_shown(visibility));
                CHECK(near_shown(visibility));
            }
        }
        for (int degrees = 40; degrees <= 44; ++degrees) {
            const auto visibility = at(static_cast<float>(degrees));
            CHECK(far_shown(visibility));
            CHECK(near_shown(visibility));
        }
        CHECK_FALSE(far_shown(at(46.f)));
        CHECK(near_shown(at(46.f)));
    };
    check_far_eye(1.f);
    check_far_eye(-1.f);

    oscilline::EyeCullState profile;
    const auto profile_at = [&](float yaw) {
        return oscilline::eye_visibility_for_yaw(yaw, profile);
    };
    for (int degrees = 60; degrees <= 90; degrees += 5) {
        const auto visibility = profile_at(yaw_of(static_cast<float>(degrees)));
        CHECK_FALSE(visibility.left);
        CHECK(visibility.right);
    }
    for (float noise : {-2.f, -0.5f, 0.f, 1.f, 2.f, 3.5f}) {
        const auto visibility = profile_at(oscilline::kEyeSideYawRadians + noise * deg);
        CHECK_FALSE(visibility.left);
        CHECK(visibility.right);
    }
    const auto behind = profile_at(oscilline::kEyeAwayEnterRadians + 0.02f);
    CHECK_FALSE(behind.left);
    CHECK_FALSE(behind.right);
    const auto still_behind = profile_at(yaw_of(147.f));
    CHECK_FALSE(still_behind.left);
    CHECK_FALSE(still_behind.right);
    for (float degrees : {180.f, 165.f, 155.f, 152.f}) {
        for (float noise : {-3.f, -1.f, 0.f, 1.5f, 2.5f}) {
            const auto visibility = profile_at(yaw_of(degrees + noise));
            CHECK_FALSE(visibility.left);
            CHECK_FALSE(visibility.right);
        }
    }
    // Back inside 146°: the near eye draws again, all the way to the side view.
    const auto turned_back = profile_at(yaw_of(140.f));
    CHECK_FALSE(turned_back.left);
    CHECK(turned_back.right);
    const auto side_again = profile_at(oscilline::kEyeSideYawRadians);
    CHECK_FALSE(side_again.left);
    CHECK(side_again.right);
    for (float noise : {-2.f, 0.f, 1.5f, 3.5f}) {
        const auto visibility = profile_at(oscilline::kEyeSideYawRadians + noise * deg);
        CHECK_FALSE(visibility.left);
        CHECK(visibility.right);
    }

    // The other side: the near eye is model -X, and it comes back at -90°.
    const auto other_behind = profile_at(-oscilline::kEyeAwayEnterRadians - 0.02f);
    CHECK_FALSE(other_behind.left);
    CHECK_FALSE(other_behind.right);
    const auto other_side = profile_at(-oscilline::kEyeSideYawRadians);
    CHECK(other_side.left);
    CHECK_FALSE(other_side.right);

    const oscilline::TmdModel model = eyed_figure();
    const oscilline::AnmFile clip = keyed_eyes();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement.valid = true;
    pose.placement.scale = 1.f;
    oscilline::EyeCullState painted;
    pose.eyes = &painted;
    const auto camera_at = [&](float degrees) {
        oscilline::AncSample camera;
        camera.fov = 500.f;
        if (degrees == 90.f) {
            camera.eye_x = 1000.f;
            camera.eye_z = 0.f;
            return camera;
        }
        const float yaw = yaw_of(degrees);
        camera.eye_x = std::sin(yaw) * 1000.f;
        camera.eye_z = std::cos(yaw) * 1000.f;
        return camera;
    };
    const auto paint_at = [&](float degrees) {
        const oscilline::AncSample camera = camera_at(degrees);
        std::vector<oscilline::Segment> lines;
        std::vector<oscilline::FilledTriangle> fills;
        CHECK(oscilline::paint_disc_figure(lines, pose, 120.f, 240.f, &fills, &camera));
        return fills;
    };
    const auto past = paint_at(50.f);
    CHECK(fills_of(past, true) == 1);
    CHECK(fills_of(past, false) == 1);
    for (float degrees : {44.f, 42.5f, 41.f, 40.2f}) {
        const auto fills = paint_at(degrees);
        CHECK(fills_of(fills, true) == 1);
        CHECK(fills_of(fills, false) == 1);
    }
    const auto shown = paint_at(39.f);
    CHECK(fills_of(shown, true) == 1);
    CHECK(fills_of(shown, false) == 1);
    const auto still = paint_at(44.f);
    CHECK(fills_of(still, true) == 1);
    CHECK(fills_of(still, false) == 1);
    const auto hidden_again = paint_at(50.f);
    CHECK(fills_of(hidden_again, true) == 1);
    CHECK(fills_of(hidden_again, false) == 1);
    const auto back = paint_at(180.f);
    CHECK(fills_of(back, true) == 1);
    CHECK(fills_of(back, false) == 1);
    const auto profile_paint = paint_at(90.f);
    CHECK(fills_of(profile_paint, true) == 1);
    CHECK(fills_of(profile_paint, false) == 1);
}

TEST_CASE("a hidden eye keeps every part when the pose wobbles") {
    oscilline::TmdModel model = eyed_figure();
    model.objects[0].vertices.push_back({-10, -72, 1});
    model.objects[0].vertices.push_back({-6, -72, 1});
    model.objects[0].vertices.push_back({-8, -66, 1});
    const auto outline = static_cast<std::uint16_t>(model.objects[0].vertices.size() - 3);
    model.objects[0].primitives.push_back(
        eye_line(outline, static_cast<std::uint16_t>(outline + 1)));
    model.objects[0].primitives.push_back(
        eye_line(static_cast<std::uint16_t>(outline + 1), static_cast<std::uint16_t>(outline + 2)));
    model.objects[0].primitives.push_back(
        eye_line(static_cast<std::uint16_t>(outline + 2), outline));
    oscilline::TmdObject pupil;
    pupil.vertices = {{0, 0, 0}, {2, 0, 0}, {1, 3, 0}};
    pupil.primitives.push_back(eye_poly(0, 1, 2, 0x000000FFu));
    model.objects.push_back(std::move(pupil));

    oscilline::AnmFile clip;
    clip.unk1 = 30;
    clip.frame_count = 1;
    clip.frames.resize(1);
    for (std::uint8_t index = 0; index < 4; ++index) {
        oscilline::AnmKeyframe key;
        key.object_index = index;
        if (index == 3) {
            key.has_position = true;
            key.position_x = -8;
            key.position_y = -70;
        }
        clip.frames[0].keys.push_back(key);
    }
    const auto posed = [&] {
        return oscilline::poses_for_frame(clip, model.objects.size(), 0, 0.f, false);
    };
    const oscilline::EyeVisibility side =
        oscilline::eye_visibility_for_yaw(oscilline::kEyeSideYawRadians);
    CHECK_FALSE(side.left);
    CHECK(side.right);

    // The outline lives on the body. Moving that object off the fill used to
    // let the line decide for itself and draw while the fill stayed hidden.
    // Bind identity keeps the line with the eye even with no remembered state.
    {
        std::vector<oscilline::Pose> shifted = posed();
        shifted[0].position_x = 40.f;
        shifted[0].rotation_z = 0.08f;
        const oscilline::TmdModel drawn = oscilline::cull_figure_eyes(shifted, model, side);
        CHECK_FALSE(shifted[1].visible);
        CHECK(shifted[2].visible);
        CHECK(drawn.objects[0].primitives.size() == 1);
    }

    oscilline::EyeCullState state;
    {
        std::vector<oscilline::Pose> settled = posed();
        const oscilline::TmdModel drawn = oscilline::cull_figure_eyes(settled, model, side, &state);
        CHECK_FALSE(settled[1].visible);
        CHECK_FALSE(settled[3].visible);
        CHECK(settled[2].visible);
        CHECK(drawn.objects[0].primitives.size() == 1);
    }
    {
        std::vector<oscilline::Pose> shifted = posed();
        shifted[0].position_x = 40.f;
        shifted[0].rotation_z = 0.08f;
        shifted[3].position_x += 1.5f;
        shifted[3].rotation_z = 0.05f;
        const oscilline::TmdModel drawn = oscilline::cull_figure_eyes(shifted, model, side, &state);
        CHECK_FALSE(shifted[1].visible);
        CHECK_FALSE(shifted[3].visible);
        CHECK(shifted[2].visible);
        CHECK(drawn.objects[0].primitives.size() == 1);
    }

    oscilline::TmdModel shaken = model;
    shaken.objects[0].vertices[2] = {80, 0, 0};
    shaken.objects[0].vertices[3] = {84, 0, 0};
    shaken.objects[0].vertices[4] = {82, 6, 0};
    std::vector<oscilline::Pose> wobble = posed();
    wobble[1].position_y = 40.f;
    wobble[1].rotation_z = 0.4f;
    wobble[3].position_x = 40.f;
    wobble[3].position_y = 40.f;
    wobble[3].rotation_z = 0.3f;

    // No memory: this frame's bounds no longer name the eyes, so the fill,
    // the pupil, and the outline all come back on their own.
    oscilline::EyeCullState fresh;
    std::vector<oscilline::Pose> unpaired = wobble;
    const oscilline::TmdModel dropped = oscilline::cull_figure_eyes(unpaired, shaken, side, &fresh);
    CHECK(unpaired[1].visible);
    CHECK(unpaired[3].visible);
    CHECK(dropped.objects[0].primitives.size() == 4);

    for (int frame = 0; frame < 8; ++frame) {
        const float noise = (frame % 2 == 0 ? 1.f : -1.f) * (0.4f + 0.2f * frame);
        std::vector<oscilline::Pose> frame_pose = wobble;
        frame_pose[3].position_x += noise;
        frame_pose[3].position_y -= noise;
        frame_pose[3].rotation_y = noise * 0.05f;
        frame_pose[1].rotation_z += noise * 0.02f;
        const oscilline::TmdModel drawn =
            oscilline::cull_figure_eyes(frame_pose, shaken, side, &state);
        CHECK_FALSE(frame_pose[1].visible);
        CHECK_FALSE(frame_pose[3].visible);
        CHECK(frame_pose[2].visible);
        CHECK(drawn.objects[0].primitives.size() == 1);
    }
}

TEST_CASE("eye hiding leaves the head, ears, and arms alone") {
    constexpr std::uint32_t kRest = 0x00808080u;
    constexpr std::uint32_t kLeft = 0x000000FFu;
    constexpr std::uint32_t kRight = 0x0000FF00u;
    constexpr std::uint32_t kOutline = 0x00FF0000u;
    oscilline::TmdModel model;

    oscilline::TmdObject body;
    body.vertices = {
        {0, 0, 0},
        {0, -80, 0},
        {-20, -80, 0},
        {20, -80, 0},
        {12, -30, 0},
        {36, -16, 0},
        {-8, -64, 1},
        {-8, -65, 1},
        {-10, -72, 1},
        {-6, -72, 1},
        {-8, -66, 1},
    };
    const auto rest_line = [](std::uint16_t a, std::uint16_t b) {
        oscilline::TmdPrimitive primitive = eye_line(a, b);
        primitive.color = kRest;
        return primitive;
    };
    body.primitives.push_back(rest_line(0, 1));
    body.primitives.push_back(rest_line(2, 3));
    body.primitives.push_back(rest_line(4, 5));
    // Beside the eye, inside the old object sphere, outside the fill itself.
    body.primitives.push_back(rest_line(6, 7));
    oscilline::TmdPrimitive outline = eye_line(8, 9);
    outline.color = kOutline;
    body.primitives.push_back(outline);
    outline = eye_line(9, 10);
    outline.color = kOutline;
    body.primitives.push_back(outline);
    outline = eye_line(10, 8);
    outline.color = kOutline;
    body.primitives.push_back(outline);

    oscilline::TmdObject left;
    left.vertices = {{-10, -72, 1}, {-6, -72, 1}, {-8, -66, 1}, {-8, -50, 1}, {-2, -48, 1}};
    left.primitives.push_back(eye_poly(0, 1, 2, kLeft));
    left.primitives.push_back(rest_line(3, 4));

    oscilline::TmdObject right;
    right.vertices = {{6, -72, 1}, {10, -72, 1}, {8, -66, 1}};
    right.primitives.push_back(eye_poly(0, 1, 2, kRight));

    oscilline::TmdObject left_ear;
    left_ear.vertices = {{-30, -62, 0}, {-22, -62, 0}, {-26, -52, 0}};
    left_ear.primitives.push_back(eye_poly(0, 1, 2, kRest));
    oscilline::TmdObject right_ear;
    right_ear.vertices = {{22, -62, 0}, {30, -62, 0}, {26, -52, 0}};
    right_ear.primitives.push_back(eye_poly(0, 1, 2, kRest));

    oscilline::TmdObject pupil;
    pupil.vertices = {{0, 0, 0}, {2, 0, 0}, {1, 3, 0}};
    pupil.primitives.push_back(eye_poly(0, 1, 2, kLeft));

    oscilline::TmdObject arm;
    arm.vertices = {{0, -20, 0}, {24, -8, 0}};
    arm.primitives.push_back(rest_line(0, 1));

    model.objects.push_back(std::move(body));
    model.objects.push_back(std::move(left));
    model.objects.push_back(std::move(right));
    model.objects.push_back(std::move(left_ear));
    model.objects.push_back(std::move(right_ear));
    model.objects.push_back(std::move(pupil));
    model.objects.push_back(std::move(arm));

    constexpr std::size_t kBody = 0;
    constexpr std::size_t kLeftEye = 1;
    constexpr std::size_t kRightEye = 2;
    constexpr std::size_t kLeftEar = 3;
    constexpr std::size_t kRightEar = 4;
    constexpr std::size_t kPupil = 5;
    constexpr std::size_t kArm = 6;
    constexpr int kRestCount = 8;

    std::vector<oscilline::Pose> rest(model.objects.size());
    for (oscilline::Pose& pose : rest) {
        pose.visible = true;
    }
    rest[kPupil].position_x = -8.f;
    rest[kPupil].position_y = -70.f;

    const auto count_color = [](const oscilline::TmdModel& drawn, std::uint32_t color) {
        int count = 0;
        for (const oscilline::TmdObject& object : drawn.objects) {
            for (const oscilline::TmdPrimitive& primitive : object.primitives) {
                if (primitive.color == color) {
                    ++count;
                }
            }
        }
        return count;
    };

    oscilline::EyeCullState state;
    const auto at = [&](float degrees) {
        const float yaw = degrees * std::numbers::pi_v<float> / 180.f;
        const oscilline::EyeVisibility visibility = oscilline::eye_visibility_for_yaw(yaw, state);
        std::vector<oscilline::Pose> frame = rest;
        const oscilline::TmdModel drawn =
            oscilline::cull_figure_eyes(frame, model, visibility, &state);
        CHECK(count_color(drawn, kRest) == kRestCount);
        CHECK(frame[kBody].visible);
        CHECK(frame[kLeftEye].visible);
        CHECK(frame[kLeftEar].visible);
        CHECK(frame[kRightEar].visible);
        CHECK(frame[kArm].visible);
        CHECK(frame[kRightEye].visible == visibility.right);
        CHECK(frame[kPupil].visible == visibility.left);
        CHECK(count_color(drawn, kLeft) == (visibility.left ? 2 : 0));
        CHECK(count_color(drawn, kRight) == (visibility.right ? 1 : 0));
        CHECK(count_color(drawn, kOutline) == (visibility.left ? 3 : 0));
        CHECK(drawn.objects[kLeftEye].primitives.size() == (visibility.left ? 2u : 1u));
        CHECK(drawn.objects[kArm].primitives.size() == 1);
        CHECK(drawn.objects[kLeftEar].primitives.size() == 1);
        CHECK(drawn.objects[kRightEar].primitives.size() == 1);
    };

    for (int degrees = -180; degrees <= 180; degrees += 5) {
        at(static_cast<float>(degrees));
    }
    for (int degrees = 180; degrees >= -180; degrees -= 5) {
        at(static_cast<float>(degrees));
    }
    for (float anchor : {0.f, 42.f, 48.f, 90.f, 96.f, 180.f}) {
        for (float noise : {-1.5f, 0.f, 1.5f}) {
            at(anchor + noise);
        }
    }
}

TEST_CASE("successful obstacle clips ignore non-clear animation cues") {
    using namespace oscilline;
    const std::vector<std::string> names = {
        "N00_SKIP.ANM", "N01_J.ANM", "N02_L.ANM", "N03_MISS.ANM"};
    const CharacterLibrary library = character_library(names, Confidence::High);
    std::vector<AnmFile> anims(names.size());
    for (AnmFile& animation : anims) {
        animation.unk1 = 10;
        animation.frames.resize(10);
    }

    CharacterClock clock;
    character_advance(clock, library, anims, 0.0, 0, false, Form::Rabbit);
    character_advance(
        clock, library, anims, 0.0, kActionLoop, false, Form::Rabbit, ClipMoment::Clear);
    REQUIRE(clock.clip == library.action[kActionLoop]);
    CHECK(clock.successful);
    CHECK(clock.oneshot);

    character_advance(
        clock, library, anims, 0.1, kActionBlock, true, Form::Rabbit, ClipMoment::Wrong);
    CHECK(clock.clip == library.action[kActionLoop]);
    CHECK(clock.successful);
    CHECK(clock.seconds == doctest::Approx(0.1));

    apply_super_transform_clip(clock, library.idle, static_cast<int>(anims.size()));
    CHECK(clock.clip == library.action[kActionLoop]);
    CHECK(clock.successful);

    character_advance(
        clock, library, anims, 0.0, kActionBlock, false, Form::Rabbit, ClipMoment::Clear);
    CHECK(clock.clip == library.action[kActionBlock]);
    CHECK(clock.successful);
    CHECK(clock.seconds == doctest::Approx(0.0));

    character_advance(
        clock, library, anims, 0.1, kActionLoop, false, Form::Rabbit, ClipMoment::Whiff);
    CHECK(clock.clip == library.action[kActionBlock]);
    CHECK(clock.successful);
    CHECK(clock.seconds == doctest::Approx(0.1));

    character_advance(clock, library, anims, 0.1, 0, false, Form::Frog, ClipMoment::None);
    CHECK(clock.form == Form::Frog);
    CHECK(clock.clip == library.action[kActionBlock]);
    CHECK(clock.successful);

    character_advance(clock, library, anims, 0.9, 0, false, Form::Frog, ClipMoment::None);
    CHECK(clock.clip == library.idle);
    CHECK_FALSE(clock.successful);
    CHECK_FALSE(clock.oneshot);

    character_advance(
        clock, library, anims, 0.0, kActionBlock, true, Form::Frog, ClipMoment::Wrong);
    CHECK(clock.clip == library.miss[0]);
    CHECK(clock.miss);
}
