// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Slot resolution against synthetic archives.

#include "oscilline/asset/fallback.hpp"
#include "oscilline/asset/map.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "support/iso_builder.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <random>
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

    void text(std::string_view value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
};

struct Named {
    std::string name;
    std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> make_pak(const std::vector<Named>& files) {
    Buf out;
    out.u32(static_cast<std::uint32_t>(files.size()));
    std::vector<std::vector<std::uint8_t>> bodies;
    bodies.reserve(files.size());
    for (const Named& file : files) {
        std::vector<std::uint8_t> body(file.name.begin(), file.name.end());
        body.push_back(0);
        while ((body.size() % 4) != 0) {
            body.push_back(0);
        }
        const auto size = static_cast<std::uint32_t>(file.data.size());
        body.push_back(static_cast<std::uint8_t>(size));
        body.push_back(static_cast<std::uint8_t>(size >> 8));
        body.push_back(static_cast<std::uint8_t>(size >> 16));
        body.push_back(static_cast<std::uint8_t>(size >> 24));
        body.insert(body.end(), file.data.begin(), file.data.end());
        while ((body.size() % 4) != 0) {
            body.push_back(0);
        }
        bodies.push_back(std::move(body));
    }
    std::uint32_t cursor = static_cast<std::uint32_t>(4 + 4 * files.size());
    for (const auto& body : bodies) {
        out.u32(cursor);
        cursor += static_cast<std::uint32_t>(body.size());
    }
    for (const auto& body : bodies) {
        out.bytes.insert(out.bytes.end(), body.begin(), body.end());
    }
    return out.bytes;
}

std::vector<std::uint8_t> line_tmd() {
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
    file.i16(4);
    file.i16(0);
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

std::vector<std::uint8_t> frames_anm(int frames) {
    if (frames < 1) {
        frames = 1;
    }
    Buf file;
    file.u16(0x8000);
    file.u16(30);
    file.u16(static_cast<std::uint16_t>(frames));
    const int header_units = 3 + frames + 1;
    for (int frame = 0; frame <= frames; ++frame) {
        file.u16(static_cast<std::uint16_t>(header_units + frame * 4));
    }
    for (int frame = 0; frame < frames; ++frame) {
        file.u8(0);
        file.u8(0x04);
        file.i16(static_cast<std::int16_t>(frame));
        file.i16(0);
        file.i16(0);
    }
    return file.bytes;
}

std::vector<std::uint8_t> one_key_anc() {
    Buf file;
    file.u16(0x8000);
    file.u16(0);
    file.u16(1);
    for (int i = 0; i < 16; ++i) {
        file.u8(0);
    }
    return file.bytes;
}

std::vector<std::uint8_t> tiny_vh() {
    Buf file;
    file.text("pBAV");
    file.u32(0);
    file.u32(0);
    file.u32(0);
    file.u16(0);
    file.u16(1);
    file.u16(0);
    file.u16(0);
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u32(0);
    for (int i = 0; i < 128 * 16; ++i) {
        file.u8(0);
    }
    for (int i = 0; i < 512; ++i) {
        file.u8(0);
    }
    for (int i = 0; i < 256; ++i) {
        file.u16(0);
    }
    return file.bytes;
}

oscilline::testutil::IsoNode file_node(std::string name, std::vector<std::uint8_t> data) {
    oscilline::testutil::IsoNode node;
    node.name = std::move(name);
    node.data = std::move(data);
    node.xa_attributes = 0x0D55;
    return node;
}

oscilline::testutil::IsoNode dir_node(std::string name) {
    oscilline::testutil::IsoNode node;
    node.name = std::move(name);
    node.directory = true;
    node.xa_attributes = 0x8D55;
    return node;
}

oscilline::testutil::IsoNode audio_dir(std::string stem) {
    auto audio = dir_node("AUDIO");
    audio.children.push_back(file_node(stem + ".VH", tiny_vh()));
    audio.children.push_back(file_node(stem + ".VB", {}));
    return audio;
}

const char* kPalBoot =
    "BOOT = cdrom:\\SCES_028.73;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n";
const char* kJpBoot =
    "BOOT = cdrom:\\SCPS_454.69;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n";

std::vector<Named> sample_entries() {
    return {
        {"FONT/FE_FONT.TMD", line_tmd()},
        {"FONT/MARK.TMD", line_tmd()},
        {"MENU/LOGOS/BOOT.TIM", {0x10, 0x00, 0x00, 0x00}},
        {"CHARA/PEELOO/MODEL.TMD", line_tmd()},
        {"CHARA/PEELOO/IDLE.ANM", frames_anm(2)},
        {"METERS/ARC.TMD", line_tmd()},
        {"METERS/ARC.ANM", frames_anm(4)},
        {"METERS/EVO.TMD", line_tmd()},
        {"METERS/EVO.ANM", frames_anm(2)},
        {"METERS/BROKEN.ANM", {0x01, 0x02}},
        {"MESSAGE/ROUND.TMD", line_tmd()},
        {"MESSAGE/ROUND.ANM", frames_anm(1)},
        {"RESULT/DIGIT.TMD", line_tmd()},
        {"ROAD/B01.ANC", one_key_anc()},
        {"TITLE/VIBRI/VIBRI.TMD", line_tmd()},
    };
}

oscilline::MountedDisc
mount_sample(std::string_view boot, std::string pak_name, std::vector<Named> entries) {
    using oscilline::testutil::IsoNode;
    IsoNode root;
    root.directory = true;
    root.children.push_back(file_node("SYSTEM.CNF", oscilline::testutil::bytes_from(boot)));
    auto game = dir_node("GAME");
    game.children.push_back(file_node(pak_name, make_pak(entries)));
    game.children.push_back(audio_dir("PSJ_SE"));
    root.children.push_back(std::move(game));
    auto title = dir_node("TITLE");
    title.children.push_back(file_node("FILES.PAK", make_pak({{"CAM/MOVE.ANC", one_key_anc()}})));
    title.children.push_back(audio_dir("TITLE"));
    root.children.push_back(std::move(title));
    auto kiosk = dir_node("KIOSK");
    kiosk.children.push_back(audio_dir("KIOSK"));
    root.children.push_back(std::move(kiosk));

    auto image =
        oscilline::DiscImage::open_iso_bytes(oscilline::testutil::build_iso(root, "OSCILLINE"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    auto identity = oscilline::identify_disc(volume.value());
    REQUIRE(identity);
    oscilline::MountedDisc disc;
    disc.image = std::move(image.value());
    disc.volume = std::move(volume.value());
    disc.identity = std::move(identity.value());
    return disc;
}

oscilline::AssetRegistry
registry_from_iso(std::string_view boot,
                  std::string pak_name,
                  std::vector<Named> entries,
                  bool force_placeholder = false,
                  oscilline::Confidence minimum = oscilline::kUsableConfidence) {
    oscilline::MountedDisc disc = mount_sample(boot, pak_name, std::move(entries));
    return oscilline::AssetRegistry::from_disc(disc, force_placeholder, minimum);
}

std::string row_line(const std::string& report, std::string_view name) {
    const auto start = report.find(name);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = report.find('\n', start);
    return report.substr(start, end - start);
}

} // namespace

TEST_CASE("the asset map is one table and skips boot logos") {
    using namespace oscilline;
    const auto rows = asset_map();
    REQUIRE(rows.size() == static_cast<std::size_t>(Slot::Count));
    bool seen[static_cast<std::size_t>(Slot::Count)] = {};
    for (const MapRow& row : rows) {
        const auto index = static_cast<std::size_t>(row.slot);
        CHECK_FALSE(seen[index]);
        seen[index] = true;
        CHECK(row.name[0] != '\0');
        CHECK(row.evidence_note[0] != '\0');
        CHECK(evidence_name(row.evidence).size() > 0);
        const std::string pattern = row.pattern;
        CHECK(pattern.find(".TIM") == std::string::npos);
        CHECK(pattern.find("LOGO") == std::string::npos);
        if (row.kind == AssetKind::Sound) {
            CHECK(row.confidence == Confidence::Low);
            CHECK(row.program < 0);
            CHECK(row.note < 0);
        }
    }
    for (bool present : seen) {
        CHECK(present);
    }
}

TEST_CASE("no disc and a forced placeholder leave every slot on the stand-in") {
    oscilline::AssetRegistry empty = oscilline::AssetRegistry::placeholders();
    CHECK(empty.report().find("disc") == std::string::npos);
    CHECK(empty.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Placeholder);
    CHECK(empty.model(oscilline::Slot::Font) == nullptr);

    auto forced = registry_from_iso(kPalBoot, "02_FILES.PAK", sample_entries(), true);
    CHECK(forced.report().find("disc") == std::string::npos);
    CHECK(forced.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Placeholder);
    CHECK(forced.warnings().empty());

    oscilline::SilentSound silent;
    silent.play(1, 2);
    CHECK(silent.plays == 1);
    const oscilline::SoundCue cue = empty.sound(oscilline::Slot::SoundPerfect);
    CHECK(cue.origin == oscilline::AssetOrigin::Placeholder);
}

TEST_CASE("custom play uses the disc map only when a disc is mounted and allowed") {
    using namespace oscilline;
    CHECK(assets_for_custom_play(nullptr, true).origin(Slot::Font) == AssetOrigin::Placeholder);
    CHECK(assets_for_custom_play(nullptr, true).origin(Slot::FormBase) == AssetOrigin::Placeholder);

    MountedDisc mounted = mount_sample(kPalBoot, "02_FILES.PAK", sample_entries());
    const AssetRegistry blocked = assets_for_custom_play(&mounted, false);
    CHECK(blocked.origin(Slot::Font) == AssetOrigin::Placeholder);
    CHECK(blocked.origin(Slot::FormBase) == AssetOrigin::Placeholder);

    const AssetRegistry used = assets_for_custom_play(&mounted, true);
    const AssetRegistry direct = AssetRegistry::from_disc(mounted, false);
    CHECK(used.origin(Slot::Font) == AssetOrigin::Disc);
    CHECK(used.origin(Slot::FormBase) == AssetOrigin::Disc);
    CHECK(used.location(Slot::Font) == direct.location(Slot::Font));
    CHECK(used.location(Slot::FormBase) == direct.location(Slot::FormBase));
    CHECK(used.origin(Slot::SoundPerfect) == direct.origin(Slot::SoundPerfect));
}

TEST_CASE("a synthetic disc resolves present slots and leaves gaps on the placeholder") {
    auto registry = registry_from_iso(kPalBoot, "02_FILES.PAK", sample_entries());
    const std::string report = registry.report();
    const std::string font = row_line(report, "font ");
    CHECK(font.find("disc") != std::string::npos);
    CHECK(font.find("GAME/02_FILES.PAK:FONT/FE_FONT.TMD") != std::string::npos);
    CHECK(report.find("LOGOS") == std::string::npos);
    CHECK(report.find(".TIM") == std::string::npos);

    CHECK(registry.origin(oscilline::Slot::FormBase) == oscilline::AssetOrigin::Disc);
    CHECK(registry.location(oscilline::Slot::FormBase).find("CHARA/PEELOO/MODEL.TMD") !=
          std::string::npos);
    CHECK(registry.origin(oscilline::Slot::FormDegradedA) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.origin(oscilline::Slot::RankMarks) == oscilline::AssetOrigin::Placeholder);
    // Coupons use MARK independently of the still-unverified results marks.
    CHECK(registry.origin(oscilline::Slot::ScoreCoupons) == oscilline::AssetOrigin::Disc);
    REQUIRE(registry.model(oscilline::Slot::ScoreCoupons) != nullptr);
    auto raised = registry_from_iso(
        kPalBoot, "02_FILES.PAK", sample_entries(), false, oscilline::Confidence::Medium);
    CHECK(raised.origin(oscilline::Slot::RankMarks) == oscilline::AssetOrigin::Disc);
    CHECK(raised.origin(oscilline::Slot::MeterEvolution) == oscilline::AssetOrigin::Disc);
    CHECK(raised.location(oscilline::Slot::MeterProgress).find("METERS/ARC.TMD") !=
          std::string::npos);
    CHECK(registry.origin(oscilline::Slot::MeterProgress) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.origin(oscilline::Slot::MeterEvolution) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.origin(oscilline::Slot::RoundCaption) == oscilline::AssetOrigin::Placeholder);
    CHECK(raised.origin(oscilline::Slot::RoundCaption) == oscilline::AssetOrigin::Disc);
    CHECK(registry.origin(oscilline::Slot::ResultsDigits) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.origin(oscilline::Slot::MenuModel) == oscilline::AssetOrigin::Disc);
    CHECK(registry.location(oscilline::Slot::MenuModel).find("TITLE/VIBRI/VIBRI.TMD") !=
          std::string::npos);
    CHECK(registry.origin(oscilline::Slot::CameraRoad) == oscilline::AssetOrigin::Disc);
    CHECK(registry.origin(oscilline::Slot::CameraMenu) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.location(oscilline::Slot::SoundBankGame).find("GAME/AUDIO/PSJ_SE.VH") !=
          std::string::npos);
    CHECK(registry.location(oscilline::Slot::SoundBankTitle).find("TITLE/AUDIO/TITLE.VH") !=
          std::string::npos);
    CHECK(registry.location(oscilline::Slot::SoundBankTutorial).find("KIOSK/AUDIO/KIOSK.VH") !=
          std::string::npos);

    registry.preload_all();
    const oscilline::TmdModel* model = registry.model(oscilline::Slot::FormBase);
    REQUIRE(model != nullptr);
    CHECK(model->objects.size() == 1);
    const auto* clips = registry.animations(oscilline::Slot::FormBase);
    REQUIRE(clips != nullptr);
    CHECK(clips->size() == 1);
    REQUIRE(registry.sound_bank(oscilline::Slot::SoundBankGame) != nullptr);
    CHECK(registry.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Disc);
}

TEST_CASE("the coupon model attaches its exact carousel animation when present") {
    using namespace oscilline;
    const auto parsed =
        parse_pak(make_pak({{"FONT/MARK.TMD", line_tmd()}, {"METERS/SCORE.ANM", frames_anm(240)}}));
    REQUIRE(parsed);
    AssetRegistry::Parts parts;
    parts.game = &parsed.value();
    auto registry = AssetRegistry::from_parts(parts);
    const auto* animations = registry.animations(Slot::ScoreCoupons);
    REQUIRE(animations != nullptr);
    REQUIRE(animations->size() == 1);
    CHECK(animations->front().frames.size() == 240);
    CHECK(registry.origin(Slot::RankMarks) == AssetOrigin::Placeholder);
}

TEST_CASE("a japanese archive prefers the 01 font file") {
    std::vector<Named> entries = sample_entries();
    entries[0].name = "FONT/01_FONT.TMD";
    auto registry = registry_from_iso(kJpBoot, "FILES.PAK", entries);
    CHECK(registry.location(oscilline::Slot::Font).find("FONT/01_FONT.TMD") != std::string::npos);
    CHECK(registry.location(oscilline::Slot::Font).find("GAME/FILES.PAK:") != std::string::npos);
}

TEST_CASE("a malformed model falls back with a warning and fuzzed bytes do not crash") {
    std::vector<Named> entries = sample_entries();
    entries[0].data = {0x00, 0x11, 0x22};
    auto registry = registry_from_iso(kPalBoot, "02_FILES.PAK", entries);
    CHECK(registry.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Disc);
    registry.load(oscilline::Slot::Font);
    CHECK(registry.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Placeholder);
    CHECK(registry.model(oscilline::Slot::Font) == nullptr);
    REQUIRE_FALSE(registry.warnings().empty());
    CHECK(registry.origin(oscilline::Slot::MeterProgress) == oscilline::AssetOrigin::Placeholder);

    std::mt19937 rng(0xA55E7u);
    for (int i = 0; i < 24; ++i) {
        std::vector<std::uint8_t> junk(1 + static_cast<std::size_t>(rng() % 40));
        for (std::uint8_t& byte : junk) {
            byte = static_cast<std::uint8_t>(rng());
        }
        oscilline::PakArchive archive;
        oscilline::PakEntry entry;
        entry.name = "FONT/FE_FONT.TMD";
        entry.data = std::move(junk);
        archive.entries.push_back(std::move(entry));
        oscilline::AssetRegistry::Parts parts;
        parts.game = &archive;
        parts.game_pak_path = "GAME/02_FILES.PAK";
        parts.region = oscilline::Region::Pal;
        auto fuzzed = oscilline::AssetRegistry::from_parts(parts);
        fuzzed.load(oscilline::Slot::Font);
        if (fuzzed.origin(oscilline::Slot::Font) == oscilline::AssetOrigin::Placeholder) {
            CHECK_FALSE(fuzzed.warnings().empty());
        }
    }
}
