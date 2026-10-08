// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// CUE, sectors, ISO 9660, and the boot-file check.

#include "oscilline/disc/cue.hpp"
#include "oscilline/disc/identity.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "oscilline/disc/sector.hpp"
#include "support/iso_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

std::filesystem::path extract_tool() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    REQUIRE(length > 0);
    return std::filesystem::path(buffer).parent_path() / "oscilline-extract.exe";
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    REQUIRE(_NSGetExecutablePath(buffer, &size) == 0);
    return std::filesystem::canonical(buffer).parent_path() / "oscilline-extract";
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path() / "oscilline-extract";
#endif
}

std::string quote_arg(const std::filesystem::path& path) {
    const std::string text = path.string();
#if defined(_WIN32)
    return "\"" + text + "\"";
#else
    return "'" + text + "'";
#endif
}

// cmd.exe strips one leading and one trailing quote when a command contains
// more than a single pair, which breaks a quoted executable plus quoted paths.
int run_shell(const std::string& command) {
#if defined(_WIN32)
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

void remove_tree(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

oscilline::testutil::IsoNode sample_tree(std::string_view boot_line) {
    using oscilline::testutil::bytes_from;
    using oscilline::testutil::IsoNode;
    IsoNode root;
    root.directory = true;
    IsoNode config;
    config.name = "SYSTEM.CNF";
    config.data = bytes_from(boot_line);
    config.xa_attributes = 0x0D55;
    IsoNode game;
    game.name = "GAME";
    game.directory = true;
    game.xa_attributes = 0x8D55;
    IsoNode note;
    note.name = "NOTE.TXT";
    note.data = bytes_from("note");
    note.xa_attributes = 0x0D55;
    game.children.push_back(std::move(note));
    root.children.push_back(std::move(config));
    root.children.push_back(std::move(game));
    return root;
}

const char* kPalBoot =
    "BOOT = cdrom:\\SCES_028.73;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n";

} // namespace

TEST_CASE("mode 1 and mode 2 user payloads") {
    std::vector<std::uint8_t> mode1(2352, 0);
    mode1[15] = 0x01;
    mode1[16] = 0x11;
    mode1[17] = 0x22;
    auto payload = oscilline::extract_user_payload(mode1, oscilline::RawSectorKind::Mode1_2352);
    REQUIRE(payload);
    CHECK(payload.value().bytes.size() == 2048);
    CHECK(payload.value().bytes[0] == 0x11);
    CHECK_FALSE(payload.value().form2);

    std::vector<std::uint8_t> form1(2352, 0);
    form1[15] = 0x02;
    form1[24] = 0x44;
    payload = oscilline::extract_user_payload(form1, oscilline::RawSectorKind::Mode2_2352);
    REQUIRE(payload);
    CHECK(payload.value().bytes[0] == 0x44);
    CHECK_FALSE(payload.value().form2);

    std::vector<std::uint8_t> form2(2352, 0);
    form2[15] = 0x02;
    form2[0x12] = 0x20;
    form2[24] = 0x55;
    payload = oscilline::extract_user_payload(form2, oscilline::RawSectorKind::Mode2_2352);
    REQUIRE(payload);
    CHECK(payload.value().form2);
    CHECK(payload.value().bytes.size() == 2324);
    CHECK(payload.value().bytes[0] == 0x55);

    std::vector<std::uint8_t> mode2336(2336, 0);
    mode2336[2] = 0x20;
    mode2336[8] = 0x66;
    payload = oscilline::extract_user_payload(mode2336, oscilline::RawSectorKind::Mode2_2336);
    REQUIRE(payload);
    CHECK(payload.value().form2);
    CHECK(payload.value().bytes[0] == 0x66);

    std::vector<std::uint8_t> short_sector(10, 0);
    CHECK_FALSE(
        oscilline::extract_user_payload(short_sector, oscilline::RawSectorKind::Mode1_2352));
}

TEST_CASE("cd-da frames are little-endian stereo") {
    std::vector<std::uint8_t> sector(2352, 0);
    sector[0] = 0x34;
    sector[1] = 0x12;
    sector[2] = 0x78;
    sector[3] = 0x56;
    auto frames = oscilline::decode_cdda_sector(sector);
    REQUIRE(frames);
    CHECK(frames.value().size() == 588);
    CHECK(frames.value()[0].left == 0x1234);
    CHECK(frames.value()[0].right == 0x5678);
}

TEST_CASE("cue parser accepts a multi-track sheet and rejects garbage") {
    const char* text = "REM this is a comment\n"
                       "FILE \"disc.bin\" BINARY\n"
                       "  TRACK 01 MODE2/2352\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    PREGAP 00:02:00\n"
                       "    INDEX 01 00:02:00\n";
    auto sheet = oscilline::parse_cue(text);
    REQUIRE(sheet);
    REQUIRE(sheet.value().tracks.size() == 2);
    CHECK(sheet.value().tracks[1].has_pregap);
    CHECK(sheet.value().tracks[1].pregap_sectors == 150);
    CHECK(sheet.value().tracks[1].indices[0].as_sectors() == 150);

    CHECK_FALSE(oscilline::parse_cue("FILE \"a.bin\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"));
    CHECK_FALSE(oscilline::parse_cue("not a cue"));
    CHECK_FALSE(oscilline::parse_cue(""));

    std::mt19937 rng(1);
    std::uniform_int_distribution<int> bytes(0, 255);
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::string junk(static_cast<std::size_t>(attempt * 7), '\0');
        for (char& ch : junk) {
            ch = static_cast<char>(bytes(rng));
        }
        auto parsed = oscilline::parse_cue(junk);
        if (!parsed) {
            CHECK_FALSE(parsed.error().empty());
        }
    }
}

TEST_CASE("plain iso identifies a pal disc and reads files") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    auto image = oscilline::DiscImage::open_iso_bytes(iso);
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    CHECK(volume.value().volume_id() == "OSCILLINE");
    CHECK(volume.value().logical_block_size() == 2048);

    auto identity = oscilline::identify_disc(volume.value());
    REQUIRE(identity);
    CHECK(identity.value().region == oscilline::Region::Pal);
    CHECK(identity.value().serial == "SCES-02873");
    CHECK(identity.value().boot_file == "SCES_028.73");

    const auto* config = volume.value().find("system.cnf");
    REQUIRE(config != nullptr);
    CHECK(config->xa.present);
    CHECK(config->xa.attributes == 0x0D55);
    CHECK(config->xa.is_mode2());
    CHECK_FALSE(config->xa.is_form2());
    CHECK_FALSE(config->xa.is_cdda());
    auto body = volume.value().read_file(*config);
    REQUIRE(body);
    CHECK(std::string(body.value().begin(), body.value().end()).find("SCES_028.73") !=
          std::string::npos);

    const auto* game = volume.value().find("/GAME");
    REQUIRE(game != nullptr);
    CHECK(game->directory);
    CHECK(game->xa.is_directory());

    const auto* note = volume.value().find("GAME/NOTE.TXT");
    REQUIRE(note != nullptr);
    auto note_body = volume.value().read_file(*note);
    REQUIRE(note_body);
    CHECK(std::string(note_body.value().begin(), note_body.value().end()) == "note");
}

TEST_CASE("japanese boot file is accepted and an unknown boot file is refused") {
    const char* jp = "boot=cdrom:/SCPS_454.69;1\n";
    auto image = oscilline::DiscImage::open_iso_bytes(
        oscilline::testutil::build_iso(sample_tree(jp), "OSCILLINE"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    auto identity = oscilline::identify_disc(volume.value());
    REQUIRE(identity);
    CHECK(identity.value().region == oscilline::Region::Japan);
    CHECK(identity.value().serial == "SCPS-45469");

    const char* unknown = "BOOT = cdrom:\\SCUS_999.99;1 ignored\r\n";
    image = oscilline::DiscImage::open_iso_bytes(
        oscilline::testutil::build_iso(sample_tree(unknown), "OSCILLINE"));
    REQUIRE(image);
    volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    identity = oscilline::identify_disc(volume.value());
    CHECK_FALSE(identity);
    CHECK(identity.error().find("SCUS_999.99") != std::string::npos);
    CHECK(identity.error().find("primary") != std::string::npos);
    CHECK(identity.error().find("SCES-02873") != std::string::npos);
    CHECK(identity.error().find("SCPS-45469") != std::string::npos);
    CHECK(identity.error().find("Vib-Ribbon") != std::string::npos);
}

TEST_CASE("mode2 bin cue with audio and a stored pregap") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    constexpr std::uint32_t kPregap = 150;
    std::vector<std::uint8_t> bin(static_cast<std::size_t>(kPregap) * 2352, 0);
    auto data = oscilline::testutil::wrap_raw_sectors(iso, 2);
    bin.insert(bin.end(), data.begin(), data.end());
    const std::uint32_t data_sectors = static_cast<std::uint32_t>(iso.size() / 2048);
    oscilline::testutil::append_cdda(bin, 2, 0x0102);

    const std::string cue = "FILE \"game.bin\" BINARY\n"
                            "  TRACK 01 MODE2/2352\n"
                            "    INDEX 00 00:00:00\n"
                            "    INDEX 01 00:02:00\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 " +
                            oscilline::testutil::msf(kPregap + data_sectors) + "\n";
    std::map<std::string, std::vector<std::uint8_t>> files;
    files.emplace("game.bin", std::move(bin));
    auto image = oscilline::DiscImage::open_cue_bytes(cue, std::move(files));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    auto identity = oscilline::identify_disc(volume.value());
    REQUIRE(identity);
    CHECK(identity.value().serial == "SCES-02873");

    REQUIRE(image.value().audio_tracks().size() == 1);
    CHECK(image.value().audio_tracks()[0].number == 2);
    CHECK(image.value().audio_tracks()[0].sector_count == 2);
    auto audio = image.value().read_audio_sector(2, 0);
    REQUIRE(audio);
    auto frames = oscilline::decode_cdda_sector(audio.value());
    REQUIRE(frames);
    CHECK(frames.value()[0].left == 0x0102);
    CHECK(frames.value()[10].right == 10);
    CHECK_FALSE(image.value().read_audio_sector(2, 2));
    CHECK_FALSE(image.value().read_logical_sector(image.value().logical_sector_count()));
}

TEST_CASE("mode1 image and a separate audio file") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    auto data = oscilline::testutil::wrap_raw_sectors(iso, 1);
    std::vector<std::uint8_t> audio;
    oscilline::testutil::append_cdda(audio, 1, 7);
    const char* cue = "FILE \"data.bin\" BINARY\n"
                      "  TRACK 01 MODE1/2352\n"
                      "    INDEX 01 00:00:00\n"
                      "FILE \"audio.bin\" BINARY\n"
                      "  TRACK 02 AUDIO\n"
                      "    INDEX 01 00:00:00\n";
    std::map<std::string, std::vector<std::uint8_t>> files;
    files.emplace("data.bin", std::move(data));
    files.emplace("audio.bin", std::move(audio));
    auto image = oscilline::DiscImage::open_cue_bytes(cue, std::move(files));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    const auto* note = volume.value().find("/GAME/NOTE.TXT");
    REQUIRE(note != nullptr);
    auto body = volume.value().read_file(*note);
    REQUIRE(body);
    CHECK(std::string(body.value().begin(), body.value().end()) == "note");
    auto sector = image.value().read_audio_sector(2, 0);
    REQUIRE(sector);
    auto frames = oscilline::decode_cdda_sector(sector.value());
    REQUIRE(frames);
    CHECK(frames.value()[0].left == 7);
}

TEST_CASE("a form 2 sector is not a logical 2048-byte sector") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    auto data = oscilline::testutil::wrap_raw_sectors(iso, 2);
    data[0x12] = 0x20;
    const char* cue = "FILE \"game.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n";
    std::map<std::string, std::vector<std::uint8_t>> files{{"game.bin", std::move(data)}};
    auto image = oscilline::DiscImage::open_cue_bytes(cue, std::move(files));
    REQUIRE(image);
    CHECK_FALSE(image.value().read_logical_sector(0));
    CHECK(image.value().read_logical_sector(16));
}

TEST_CASE("truncated images and missing boot files fail cleanly") {
    CHECK_FALSE(oscilline::DiscImage::open_iso_bytes({1, 2, 3, 4}));
    std::vector<std::uint8_t> junk(2048 * 20, 0);
    CHECK_FALSE(oscilline::DiscImage::open_iso_bytes(std::move(junk)));

    oscilline::testutil::IsoNode root;
    root.directory = true;
    auto image =
        oscilline::DiscImage::open_iso_bytes(oscilline::testutil::build_iso(root, "EMPTY"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    auto identity = oscilline::identify_disc(volume.value());
    CHECK_FALSE(identity);
    CHECK(identity.error().find("SYSTEM.CNF") != std::string::npos);
}

TEST_CASE("a raw mode 2 bin without a cue still opens") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    const auto bin = oscilline::testutil::wrap_raw_sectors(iso, 2);
    const auto directory = std::filesystem::temp_directory_path() / "oscilline-raw-bin";
    remove_tree(directory);
    std::filesystem::create_directories(directory);
    const auto bin_path = directory / "only.bin";
    {
        std::ofstream out(bin_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bin.data()),
                  static_cast<std::streamsize>(bin.size()));
    }
    {
        // The image keeps the file open. Close it before deleting the directory.
        auto image = oscilline::DiscImage::open(bin_path);
        REQUIRE(image);
        auto volume = oscilline::IsoVolume::read(image.value());
        REQUIRE(volume);
        auto identity = oscilline::identify_disc(volume.value());
        REQUIRE(identity);
        CHECK(identity.value().serial == "SCES-02873");
    }
    remove_tree(directory);
}

TEST_CASE("extract cli lists and writes a synthetic cue") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    auto bin = oscilline::testutil::wrap_raw_sectors(iso, 2);
    const auto directory = std::filesystem::temp_directory_path() / "oscilline-disc-test";
    remove_tree(directory);
    std::filesystem::create_directories(directory);
    const auto bin_path = directory / "disc.bin";
    const auto cue_path = directory / "disc.cue";
    {
        std::ofstream out(bin_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bin.data()),
                  static_cast<std::streamsize>(bin.size()));
    }
    {
        std::ofstream out(cue_path);
        out << "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    }

    const auto tool = extract_tool();
    const auto listing = directory / "list.txt";
    const std::string list_command =
        quote_arg(tool) + " list " + quote_arg(cue_path) + " > " + quote_arg(listing);
    REQUIRE(run_shell(list_command) == 0);
    std::stringstream buffer;
    {
        std::ifstream listed(listing);
        buffer << listed.rdbuf();
    }
    const std::string text = buffer.str();
    CHECK(text.find("PAL SCES-02873") != std::string::npos);
    CHECK(text.find("SYSTEM.CNF") != std::string::npos);
    CHECK(text.find("NOTE.TXT") != std::string::npos);

    const auto outdir = directory / "out";
    const std::string extract_command =
        quote_arg(tool) + " extract " + quote_arg(cue_path) + " " + quote_arg(outdir);
    REQUIRE(run_shell(extract_command) == 0);
    std::string note_text;
    {
        std::ifstream note(outdir / "GAME" / "NOTE.TXT");
        std::getline(note, note_text);
    }
    CHECK(note_text == "note");

    const auto unknown =
        oscilline::testutil::build_iso(sample_tree("BOOT = cdrom:\\SCUS_000.00;1\r\n"), "NOPE");
    const auto bad_iso = directory / "bad.iso";
    {
        std::ofstream out(bad_iso, std::ios::binary);
        out.write(reinterpret_cast<const char*>(unknown.data()),
                  static_cast<std::streamsize>(unknown.size()));
    }
    const auto bad_list = directory / "bad.txt";
    const std::string bad_command =
        quote_arg(tool) + " list " + quote_arg(bad_iso) + " > " + quote_arg(bad_list) + " 2>&1";
    CHECK(run_shell(bad_command) != 0);

    remove_tree(directory);
}

TEST_CASE("volume space may cover audio tracks that follow the data track") {
    const auto iso = oscilline::testutil::build_iso(sample_tree(kPalBoot), "OSCILLINE");
    const auto data_sectors = static_cast<std::uint32_t>(iso.size() / 2048);
    constexpr std::uint32_t kAudioA = 4;
    constexpr std::uint32_t kAudioB = 3;
    auto bin = oscilline::testutil::wrap_raw_sectors(iso, 2);
    oscilline::testutil::append_cdda(bin, kAudioA, 0x1111);
    oscilline::testutil::append_cdda(bin, kAudioB, 0x2222);
    const std::uint32_t volume_space = data_sectors + kAudioA + kAudioB;
    oscilline::testutil::write_u32_both(
        bin.data() + static_cast<std::size_t>(16) * 2352 + 24 + 0x50, volume_space);

    const std::string cue = "FILE \"game.bin\" BINARY\n"
                            "  TRACK 01 MODE2/2352\n"
                            "    INDEX 01 00:00:00\n"
                            "  TRACK 02 AUDIO\n"
                            "    INDEX 01 " +
                            oscilline::testutil::msf(data_sectors) +
                            "\n"
                            "  TRACK 03 AUDIO\n"
                            "    INDEX 01 " +
                            oscilline::testutil::msf(data_sectors + kAudioA) + "\n";
    std::map<std::string, std::vector<std::uint8_t>> files;
    files.emplace("game.bin", bin);
    auto image = oscilline::DiscImage::open_cue_bytes(cue, std::move(files));
    REQUIRE(image);
    CHECK(image.value().logical_sector_count() == data_sectors);
    CHECK(image.value().audio_tracks().size() == 2);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    CHECK(volume.value().volume_space_sectors() == volume_space);
    CHECK(volume.value().volume_space_sectors() > image.value().logical_sector_count());
    auto identity = oscilline::identify_disc(volume.value());
    REQUIRE(identity);
    CHECK(identity.value().serial == "SCES-02873");
    const auto* note = volume.value().find("GAME/NOTE.TXT");
    REQUIRE(note != nullptr);
    auto body = volume.value().read_file(*note);
    REQUIRE(body);
    CHECK(std::string(body.value().begin(), body.value().end()) == "note");
    CHECK_FALSE(image.value().read_logical_sector(data_sectors));
    auto audio = image.value().read_audio_sector(3, 0);
    REQUIRE(audio);
    auto frames = oscilline::decode_cdda_sector(audio.value());
    REQUIRE(frames);
    CHECK(frames.value()[0].left == 0x2222);

    const std::string marker = "NOTE.TXT;1";
    const auto found = std::search(bin.begin(), bin.end(), marker.begin(), marker.end());
    REQUIRE(found != bin.end());
    const std::size_t name_at = static_cast<std::size_t>(std::distance(bin.begin(), found));
    REQUIRE(name_at >= 33);
    REQUIRE(bin[name_at - 1] == marker.size());
    oscilline::testutil::write_u32_both(bin.data() + name_at - 33 + 2, data_sectors + 1);
    std::map<std::string, std::vector<std::uint8_t>> patched;
    patched.emplace("game.bin", bin);
    auto kept = oscilline::DiscImage::open_cue_bytes(cue, std::move(patched));
    REQUIRE(kept);
    auto kept_volume = oscilline::IsoVolume::read(kept.value());
    REQUIRE(kept_volume);
    auto kept_identity = oscilline::identify_disc(kept_volume.value());
    REQUIRE(kept_identity);
    CHECK(kept_identity.value().serial == "SCES-02873");
    const auto* outside = kept_volume.value().find("GAME/NOTE.TXT");
    REQUIRE(outside != nullptr);
    CHECK(outside->outside_data_track);
    auto outside_body = kept_volume.value().read_file(*outside);
    CHECK_FALSE(outside_body);
    CHECK(outside_body.error().find("past the data track") != std::string::npos);
    const auto* config = kept_volume.value().find("SYSTEM.CNF");
    REQUIRE(config != nullptr);
    CHECK_FALSE(config->outside_data_track);
    REQUIRE(kept_volume.value().read_file(*config));

    const auto directory = std::filesystem::temp_directory_path() / "oscilline-cdda-entry";
    remove_tree(directory);
    std::filesystem::create_directories(directory);
    const auto bin_path = directory / "game.bin";
    const auto cue_path = directory / "game.cue";
    {
        std::ofstream out(bin_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bin.data()),
                  static_cast<std::streamsize>(bin.size()));
    }
    {
        std::ofstream out(cue_path);
        out << cue;
    }
    const auto outdir = directory / "out";
    const auto log = directory / "extract.log";
    const std::string command = quote_arg(extract_tool()) + " extract " + quote_arg(cue_path) +
                                " " + quote_arg(outdir) + " > " + quote_arg(log) + " 2>&1";
    CHECK(run_shell(command) == 0);
    CHECK(std::filesystem::exists(outdir / "SYSTEM.CNF"));
    CHECK_FALSE(std::filesystem::exists(outdir / "GAME" / "NOTE.TXT"));
    std::stringstream extract_log;
    {
        std::ifstream logged(log);
        extract_log << logged.rdbuf();
    }
    const std::string extract_text = extract_log.str();
    CHECK(extract_text.find("NOTE.TXT") != std::string::npos);
    CHECK(extract_text.find("past the data track") != std::string::npos);
    remove_tree(directory);
}

TEST_CASE("extract keeps going when a form 2 file cannot be read") {
    auto tree = sample_tree(kPalBoot);
    oscilline::testutil::IsoNode stream;
    stream.name = "STREAM.XA";
    stream.data = oscilline::testutil::bytes_from("form2-payload");
    stream.xa_attributes = 0x0D55;
    tree.children.push_back(std::move(stream));
    const auto iso = oscilline::testutil::build_iso(tree, "OSCILLINE");
    auto plain = oscilline::DiscImage::open_iso_bytes(iso);
    REQUIRE(plain);
    auto plain_volume = oscilline::IsoVolume::read(plain.value());
    REQUIRE(plain_volume);
    const auto* stream_node = plain_volume.value().find("STREAM.XA");
    REQUIRE(stream_node != nullptr);
    REQUIRE_FALSE(stream_node->xa.is_form2());

    auto bin = oscilline::testutil::wrap_raw_sectors(iso, 2);
    const std::uint32_t sectors = stream_node->size == 0 ? 0 : (stream_node->size + 2047u) / 2048u;
    REQUIRE(sectors >= 1);
    for (std::uint32_t sector = 0; sector < sectors; ++sector) {
        bin[(static_cast<std::size_t>(stream_node->lba) + sector) * 2352 + 0x12] = 0x20;
    }

    const auto directory = std::filesystem::temp_directory_path() / "oscilline-form2-extract";
    remove_tree(directory);
    std::filesystem::create_directories(directory);
    const auto bin_path = directory / "disc.bin";
    const auto cue_path = directory / "disc.cue";
    {
        std::ofstream out(bin_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bin.data()),
                  static_cast<std::streamsize>(bin.size()));
    }
    {
        std::ofstream out(cue_path);
        out << "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    }

    const auto outdir = directory / "out";
    const auto log = directory / "extract.log";
    const std::string command = quote_arg(extract_tool()) + " extract " + quote_arg(cue_path) +
                                " " + quote_arg(outdir) + " > " + quote_arg(log) + " 2>&1";
    CHECK(run_shell(command) != 0);
    std::string note_text;
    {
        std::ifstream note(outdir / "GAME" / "NOTE.TXT");
        std::getline(note, note_text);
    }
    CHECK(note_text == "note");
    CHECK_FALSE(std::filesystem::exists(outdir / "STREAM.XA"));
    std::stringstream buffer;
    {
        std::ifstream logged(log);
        buffer << logged.rdbuf();
    }
    const std::string text = buffer.str();
    CHECK(text.find("STREAM.XA") != std::string::npos);
    CHECK(text.find("not a 2048-byte data sector") != std::string::npos);

    remove_tree(directory);
}
