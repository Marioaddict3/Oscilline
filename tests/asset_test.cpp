// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// PAK, TIM, TMD, ANM, FSL, and VH/VB parsers on synthetic bytes.

#include "oscilline/adpcm.hpp"
#include "oscilline/anm.hpp"
#include "oscilline/fsl.hpp"
#include "oscilline/inspect.hpp"
#include "oscilline/pak.hpp"
#include "oscilline/tim.hpp"
#include "oscilline/tmd.hpp"
#include "oscilline/vab.hpp"

#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

    void zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
};

std::filesystem::path inspect_tool() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    REQUIRE(length > 0);
    return std::filesystem::path(buffer).parent_path() / "oscilline-inspect.exe";
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    REQUIRE(_NSGetExecutablePath(buffer, &size) == 0);
    return std::filesystem::canonical(buffer).parent_path() / "oscilline-inspect";
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path() / "oscilline-inspect";
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

Buf sample_pak() {
    Buf file;
    file.u32(2);
    file.u32(12);
    file.u32(28);
    file.text("A.TIM");
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u32(2);
    file.text("hi");
    file.u8(0);
    file.u8(0);
    file.text("ABCD");
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u8(0);
    file.u32(4);
    file.text("test");
    return file;
}

void fuzz_parsers(const std::vector<std::uint8_t>& bytes) {
    auto pak = oscilline::parse_pak(bytes);
    if (!pak) {
        CHECK_FALSE(pak.error().empty());
    }
    auto tim = oscilline::parse_tim(bytes);
    if (!tim) {
        CHECK_FALSE(tim.error().empty());
    }
    auto tmd = oscilline::parse_tmd(bytes);
    if (!tmd) {
        CHECK_FALSE(tmd.error().empty());
    }
    auto anm = oscilline::parse_anm(bytes);
    if (!anm) {
        CHECK_FALSE(anm.error().empty());
    }
    auto vh = oscilline::parse_vh(bytes);
    if (!vh) {
        CHECK_FALSE(vh.error().empty());
    }
    auto fsl = oscilline::parse_fsl(bytes);
    if (!fsl) {
        CHECK_FALSE(fsl.error().empty());
    }
    auto adpcm = oscilline::decode_spu_adpcm(bytes);
    if (!adpcm) {
        CHECK_FALSE(adpcm.error().empty());
    }
}

} // namespace

TEST_CASE("pak reads names, padding, and payloads") {
    const auto file = sample_pak();
    auto archive = oscilline::parse_pak(file.bytes);
    REQUIRE(archive);
    REQUIRE(archive.value().entries.size() == 2);
    CHECK(archive.value().entries[0].name == "A.TIM");
    CHECK(archive.value().entries[0].size == 2);
    CHECK(std::string(archive.value().entries[0].data.begin(),
                      archive.value().entries[0].data.end()) == "hi");
    CHECK(archive.value().entries[1].name == "ABCD");
    CHECK(archive.value().entries[1].size == 4);
    CHECK(std::string(archive.value().entries[1].data.begin(),
                      archive.value().entries[1].data.end()) == "test");

    Buf empty;
    empty.u32(0);
    auto none = oscilline::parse_pak(empty.bytes);
    REQUIRE(none);
    CHECK(none.value().entries.empty());

    oscilline::InspectRequest request;
    request.type = "pak";
    request.bytes = file.bytes;
    auto json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\"name\": \"A.TIM\"") != std::string::npos);
    CHECK(json.value().find("\"data\"") == std::string::npos);

    Buf high;
    high.u32(1);
    high.u32(8);
    high.u8('A');
    high.u8(0xE9);
    high.u8(0);
    high.u8(0);
    high.u32(0);
    request.bytes = high.bytes;
    json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\\u00e9") != std::string::npos);
    CHECK(json.value().find('\xE9') == std::string::npos);
}

TEST_CASE("tim expands 4, 8, 16, and 24 bpp") {
    Buf indexed;
    indexed.u32(0x10);
    indexed.u32(8);
    indexed.u32(20);
    indexed.i16(0);
    indexed.i16(0);
    indexed.u16(4);
    indexed.u16(1);
    indexed.u16(0x0000);
    indexed.u16(0x001F);
    indexed.u16(0x03E0);
    indexed.u16(0x7C00);
    indexed.u32(14);
    indexed.i16(0);
    indexed.i16(0);
    indexed.u16(1);
    indexed.u16(1);
    indexed.u8(0x21);
    indexed.u8(0x03);
    auto image = oscilline::parse_tim(indexed.bytes);
    REQUIRE(image);
    CHECK(image.value().bpp == 4);
    CHECK(image.value().width == 4);
    CHECK(image.value().height == 1);
    REQUIRE(image.value().pixels.size() == 4);
    CHECK(image.value().pixels[0].r == 255);
    CHECK(image.value().pixels[0].a == 255);
    CHECK(image.value().pixels[1].g == 255);
    CHECK(image.value().pixels[2].b == 255);
    CHECK(image.value().pixels[3].a == 0);

    Buf eight;
    eight.u32(0x10);
    eight.u32(9);
    eight.u32(16);
    eight.i16(0);
    eight.i16(0);
    eight.u16(2);
    eight.u16(1);
    eight.u16(0x0000);
    eight.u16(0x001F);
    eight.u32(14);
    eight.i16(0);
    eight.i16(0);
    eight.u16(1);
    eight.u16(1);
    eight.u8(0x01);
    eight.u8(0x00);
    image = oscilline::parse_tim(eight.bytes);
    REQUIRE(image);
    CHECK(image.value().bpp == 8);
    CHECK(image.value().width == 2);
    REQUIRE(image.value().pixels.size() == 2);
    CHECK(image.value().pixels[0].r == 255);
    CHECK(image.value().pixels[1].a == 0);

    Buf direct;
    direct.u32(0x10);
    direct.u32(2);
    direct.u32(20);
    direct.i16(0);
    direct.i16(0);
    direct.u16(4);
    direct.u16(1);
    direct.u16(0x801F);
    direct.u16(0x001F);
    direct.u16(0x8000);
    direct.u16(0x0000);
    image = oscilline::parse_tim(direct.bytes);
    REQUIRE(image);
    CHECK(image.value().bpp == 16);
    REQUIRE(image.value().pixels.size() == 4);
    CHECK(image.value().pixels[0].r == 255);
    CHECK(image.value().pixels[0].a == 128);
    CHECK(image.value().pixels[1].r == 255);
    CHECK(image.value().pixels[1].a == 255);
    CHECK(image.value().pixels[2].a == 255);
    CHECK(image.value().pixels[2].r == 0);
    CHECK(image.value().pixels[3].a == 0);

    Buf truecolor;
    truecolor.u32(0x10);
    truecolor.u32(3);
    truecolor.u32(18);
    truecolor.i16(0);
    truecolor.i16(0);
    truecolor.u16(3);
    truecolor.u16(1);
    truecolor.u8(10);
    truecolor.u8(20);
    truecolor.u8(30);
    truecolor.u8(40);
    truecolor.u8(50);
    truecolor.u8(60);
    image = oscilline::parse_tim(truecolor.bytes);
    REQUIRE(image);
    CHECK(image.value().bpp == 24);
    CHECK(image.value().width == 2);
    REQUIRE(image.value().pixels.size() == 2);
    CHECK(image.value().pixels[0].r == 10);
    CHECK(image.value().pixels[0].g == 20);
    CHECK(image.value().pixels[0].b == 30);
    CHECK(image.value().pixels[0].a == 255);
    CHECK(image.value().pixels[1].r == 40);
    CHECK(image.value().pixels[1].b == 60);

    Buf bad;
    bad.u32(0);
    CHECK_FALSE(oscilline::parse_tim(bad.bytes));
}

TEST_CASE("tmd reads fixp offsets, lines, and raw rectangles") {
    Buf triangle;
    triangle.u32(0x41);
    triangle.u32(0);
    triangle.u32(1);
    triangle.u32(0x1C);
    triangle.u32(1);
    triangle.u32(0x24);
    triangle.u32(1);
    triangle.u32(0x2C);
    triangle.u32(1);
    triangle.i32(-3);
    triangle.i16(9);
    triangle.i16(8);
    triangle.i16(7);
    triangle.u16(0);
    triangle.i16(1);
    triangle.i16(0);
    triangle.i16(0);
    triangle.u16(0);
    triangle.u8(4);
    triangle.u8(3);
    triangle.u8(0);
    triangle.u8(0x20);
    triangle.u32(0x00FF0000);
    triangle.u16(0);
    triangle.u16(0);
    triangle.u16(1);
    triangle.u16(2);
    auto model = oscilline::parse_tmd(triangle.bytes);
    REQUIRE(model);
    CHECK_FALSE(model.value().fixp);
    REQUIRE(model.value().objects.size() == 1);
    CHECK(model.value().objects[0].scale == -3);
    REQUIRE(model.value().objects[0].vertices.size() == 1);
    CHECK(model.value().objects[0].vertices[0].x == 9);
    CHECK(model.value().objects[0].vertices[0].z == 7);
    REQUIRE(model.value().objects[0].primitives.size() == 1);
    const auto& face = model.value().objects[0].primitives[0];
    CHECK(face.kind == oscilline::PrimitiveKind::Polygon);
    CHECK(face.vertex_indices.size() == 3);
    CHECK(face.vertex_indices[2] == 2);
    CHECK(face.normal_indices.size() == 1);
    CHECK(face.normal_indices[0] == 0);
    CHECK_FALSE(face.unlit);

    triangle.bytes[4] = 1;
    triangle.bytes[12] = 0x28;
    triangle.bytes[13] = 0;
    triangle.bytes[20] = 0x30;
    triangle.bytes[21] = 0;
    triangle.bytes[28] = 0x38;
    triangle.bytes[29] = 0;
    model = oscilline::parse_tmd(triangle.bytes);
    REQUIRE(model);
    CHECK(model.value().fixp);
    CHECK(model.value().objects[0].vertices[0].x == 9);

    Buf padded = triangle;
    padded.bytes[4] = 0;
    padded.bytes[12] = 0x1C;
    padded.bytes[20] = 0x24;
    padded.bytes[28] = 0x2C;
    padded.bytes[0x39] = 4;
    padded.u32(0xABABABAB);
    model = oscilline::parse_tmd(padded.bytes);
    REQUIRE(model);
    CHECK(model.value().objects[0].primitives[0].vertex_indices[1] == 1);
    CHECK(model.value().objects[0].primitives[0].raw.size() == 20);

    Buf line;
    line.u32(0x41);
    line.u32(0);
    line.u32(1);
    line.u32(0);
    line.u32(0);
    line.u32(0);
    line.u32(0);
    line.u32(0x1C);
    line.u32(1);
    line.i32(0);
    line.u8(3);
    line.u8(2);
    line.u8(1);
    line.u8(0x40);
    line.u32(0x000000AA);
    line.u16(4);
    line.u16(5);
    model = oscilline::parse_tmd(line.bytes);
    REQUIRE(model);
    REQUIRE(model.value().objects[0].primitives.size() == 1);
    CHECK(model.value().objects[0].primitives[0].kind == oscilline::PrimitiveKind::Line);
    CHECK(model.value().objects[0].primitives[0].vertex_indices[0] == 4);
    CHECK(model.value().objects[0].primitives[0].vertex_indices[1] == 5);
    CHECK(model.value().objects[0].primitives[0].normal_indices.empty());
    CHECK(model.value().objects[0].primitives[0].extra_colors.empty());

    Buf shaded;
    shaded.u32(0x41);
    shaded.u32(0);
    shaded.u32(1);
    shaded.u32(0);
    shaded.u32(0);
    shaded.u32(0);
    shaded.u32(0);
    shaded.u32(0x1C);
    shaded.u32(1);
    shaded.i32(0);
    shaded.u8(4);
    shaded.u8(3);
    shaded.u8(0);
    shaded.u8(0x50);
    shaded.u32(1);
    shaded.u32(2);
    shaded.u16(4);
    shaded.u16(7);
    model = oscilline::parse_tmd(shaded.bytes);
    REQUIRE(model);
    CHECK(model.value().objects[0].primitives[0].kind == oscilline::PrimitiveKind::Line);
    CHECK(model.value().objects[0].primitives[0].gouraud);
    REQUIRE(model.value().objects[0].primitives[0].extra_colors.size() == 1);
    CHECK(model.value().objects[0].primitives[0].extra_colors[0] == 2);
    CHECK(model.value().objects[0].primitives[0].vertex_indices[0] == 4);

    Buf rect;
    rect.u32(0x41);
    rect.u32(0);
    rect.u32(1);
    rect.u32(0);
    rect.u32(0);
    rect.u32(0);
    rect.u32(0);
    rect.u32(0x1C);
    rect.u32(1);
    rect.i32(0);
    rect.u8(2);
    rect.u8(1);
    rect.u8(0);
    rect.u8(0x60);
    rect.u32(0x11223344);
    model = oscilline::parse_tmd(rect.bytes);
    REQUIRE(model);
    CHECK(model.value().objects[0].primitives[0].kind == oscilline::PrimitiveKind::Rectangle);
    CHECK(model.value().objects[0].primitives[0].vertex_indices.empty());
    CHECK(model.value().objects[0].primitives[0].raw.size() == 8);

    // Two objects. FIXP = 0 offsets are from the object table at 0x0C, so the
    // second object's vertex offset is not measured from its own entry.
    Buf pair;
    pair.u32(0x41);
    pair.u32(0);
    pair.u32(2);
    pair.u32(0);
    pair.u32(0);
    pair.u32(0);
    pair.u32(0);
    pair.u32(0x38);
    pair.u32(1);
    pair.i32(0);
    pair.u32(0x44);
    pair.u32(1);
    pair.u32(0);
    pair.u32(0);
    pair.u32(0);
    pair.u32(0);
    pair.i32(1);
    pair.u8(2);
    pair.u8(2);
    pair.u8(1);
    pair.u8(0x40);
    pair.u32(0xAA);
    pair.u16(1);
    pair.u16(2);
    pair.i16(3);
    pair.i16(4);
    pair.i16(5);
    pair.u16(0);
    model = oscilline::parse_tmd(pair.bytes);
    REQUIRE(model);
    CHECK_FALSE(model.value().fixp);
    REQUIRE(model.value().objects.size() == 2);
    CHECK(model.value().objects[0].primitives[0].kind == oscilline::PrimitiveKind::Line);
    CHECK(model.value().objects[0].primitives[0].vertex_indices[0] == 1);
    REQUIRE(model.value().objects[1].vertices.size() == 1);
    CHECK(model.value().objects[1].vertices[0].x == 3);
    CHECK(model.value().objects[1].vertices[0].z == 5);

    pair.bytes[4] = 1;
    pair.bytes[0x1C] = 0x44;
    pair.bytes[0x1D] = 0;
    pair.bytes[0x1E] = 0;
    pair.bytes[0x1F] = 0;
    pair.bytes[0x28] = 0x50;
    pair.bytes[0x29] = 0;
    pair.bytes[0x2A] = 0;
    pair.bytes[0x2B] = 0;
    model = oscilline::parse_tmd(pair.bytes);
    REQUIRE(model);
    CHECK(model.value().fixp);
    CHECK(model.value().objects[1].vertices[0].x == 3);
}

TEST_CASE("anm reads frame keys and rejects a key that crosses the frame") {
    Buf file;
    file.u16(0x8000);
    file.i16(30);
    file.u16(1);
    file.u16(5);
    file.u16(15);
    file.u8(3);
    file.u8(0x07);
    file.i16(1);
    file.i16(2);
    file.i16(3);
    file.i16(4096);
    file.i16(4096);
    file.i16(4096);
    file.i16(4);
    file.i16(5);
    file.i16(6);
    auto animation = oscilline::parse_anm(file.bytes);
    REQUIRE(animation);
    CHECK(animation.value().unk1 == 30);
    CHECK(animation.value().frame_count == 1);
    REQUIRE(animation.value().frames[0].keys.size() == 1);
    const auto& key = animation.value().frames[0].keys[0];
    CHECK(key.object_index == 3);
    CHECK(key.has_rotation);
    CHECK(key.has_scale);
    CHECK(key.has_position);
    CHECK(key.rotation_y == 2);
    CHECK(key.scale_x == 4096);
    CHECK(key.position_z == 6);

    Buf bare;
    bare.u16(0x8000);
    bare.i16(1);
    bare.u16(1);
    bare.u16(5);
    bare.u16(6);
    bare.u8(1);
    bare.u8(0);
    animation = oscilline::parse_anm(bare.bytes);
    REQUIRE(animation);
    REQUIRE(animation.value().frames[0].keys.size() == 1);
    CHECK_FALSE(animation.value().frames[0].keys[0].has_rotation);

    Buf crossed;
    crossed.u16(0x8000);
    crossed.i16(30);
    crossed.u16(1);
    crossed.u16(5);
    crossed.u16(8);
    crossed.u8(3);
    crossed.u8(0x01);
    crossed.i16(1);
    crossed.i16(2);
    crossed.i16(3);
    animation = oscilline::parse_anm(crossed.bytes);
    CHECK_FALSE(animation);
    CHECK(animation.error().find("crosses") != std::string::npos);
}

TEST_CASE("spu adpcm matches the documented filter coefficients") {
    std::vector<std::uint8_t> block(16, 0);
    block[2] = 0x01;
    auto pcm = oscilline::decode_spu_adpcm(block);
    REQUIRE(pcm);
    CHECK(pcm.value().samples[0] == 4096);

    block.assign(16, 0);
    block[0] = 0x1C;
    block[2] = 0x01;
    pcm = oscilline::decode_spu_adpcm(block);
    REQUIRE(pcm);
    CHECK(pcm.value().samples[0] == 1);
    CHECK(pcm.value().samples[1] == 1);

    block.assign(16, 0);
    block[0] = 0x2C;
    block[2] = 0x21;
    pcm = oscilline::decode_spu_adpcm(block);
    REQUIRE(pcm);
    CHECK(pcm.value().samples[0] == 1);
    CHECK(pcm.value().samples[1] == 4);

    block.assign(16, 0);
    block[0] = 13;
    block[2] = 0x01;
    pcm = oscilline::decode_spu_adpcm(block);
    REQUIRE(pcm);
    CHECK(pcm.value().samples[0] == 8);

    block[0] = 0x50;
    CHECK_FALSE(oscilline::decode_spu_adpcm(block));
    CHECK_FALSE(oscilline::decode_spu_adpcm(std::vector<std::uint8_t>(15, 0)));
}

TEST_CASE("vh and vb decode the first vag from the size table") {
    Buf header;
    header.text("pBAV");
    header.u32(7);
    header.u32(3);
    header.u32(100);
    header.u16(0xEEEE);
    header.u16(1);
    header.u16(4);
    header.u16(2);
    header.u8(0x7F);
    header.u8(0x40);
    header.u8(1);
    header.u8(2);
    header.u32(0xFFFFFFFF);
    header.u8(1);
    header.u8(100);
    header.u8(5);
    header.u8(1);
    header.u8(64);
    header.u8(0);
    header.u16(0x1234);
    header.zeros(8);
    header.zeros(127 * 16);
    header.u8(9);
    header.u8(4);
    header.u8(80);
    header.u8(64);
    header.u8(60);
    header.u8(0);
    header.u8(0);
    header.u8(127);
    header.zeros(6);
    header.u8(0);
    header.u8(0);
    header.u16(0x1111);
    header.u16(0x2222);
    header.u16(0);
    header.u16(0);
    header.zeros(8);
    header.zeros(15 * 32);
    header.u16(2);
    header.zeros(255 * 2);

    auto parsed = oscilline::parse_vh(header.bytes);
    REQUIRE(parsed);
    CHECK(parsed.value().version == 7);
    CHECK(parsed.value().program_count == 1);
    CHECK(parsed.value().program_count_field == 1);
    CHECK(parsed.value().programs.size() == 128);
    CHECK(parsed.value().programs[0].tone_count == 1);
    CHECK(parsed.value().programs[0].volume == 100);
    CHECK(parsed.value().programs[0].attribute == 0x1234);
    CHECK(parsed.value().programs[1].tone_count == 0);
    CHECK(parsed.value().tone_count_field == 4);
    CHECK(parsed.value().vag_count_field == 2);
    CHECK(parsed.value().tones.size() == 16);
    CHECK(parsed.value().tones[0].priority == 9);
    CHECK(parsed.value().tones[0].adsr1 == 0x1111);
    CHECK(parsed.value().vag_sizes[0] == 2);
    CHECK(parsed.value().vag_sizes[1] == 0);

    std::vector<std::uint8_t> body(16, 0);
    body[2] = 0x01;
    auto bank = oscilline::parse_vab(header.bytes, body);
    REQUIRE(bank);
    CHECK(bank.value().has_body);
    REQUIRE(bank.value().vags.size() == 256);
    CHECK(bank.value().vags[0].pcm.size() == 28);
    CHECK(bank.value().vags[0].pcm[0] == 4096);
    CHECK(bank.value().vags[1].pcm.empty());

    std::vector<std::uint8_t> extra = body;
    extra.push_back(0);
    auto mismatched = oscilline::parse_vab(header.bytes, extra);
    CHECK_FALSE(mismatched);
    CHECK(mismatched.error().find("does not match") != std::string::npos);

    header.bytes[0x12] = 0;
    CHECK_FALSE(oscilline::parse_vh(header.bytes));
    header.bytes[0x12] = 1;

    Buf trailing = header;
    trailing.u8(0);
    auto trailing_parsed = oscilline::parse_vh(trailing.bytes);
    CHECK_FALSE(trailing_parsed);
    CHECK(trailing_parsed.error().find("end of the file") != std::string::npos);

    oscilline::InspectRequest request;
    request.type = "vh";
    request.bytes = header.bytes;
    request.body = body;
    auto json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\"programs\"") != std::string::npos);
    CHECK(json.value().find("\"attribute\": 4660") != std::string::npos);
    CHECK(json.value().find("\"tone_count\": 1") != std::string::npos);
    CHECK(json.value().find("\"center\": 60") != std::string::npos);
    CHECK(json.value().find("\"shift\": 0") != std::string::npos);
    CHECK(json.value().find("\"note_min\": 0") != std::string::npos);
    CHECK(json.value().find("\"note_max\": 127") != std::string::npos);
    CHECK(json.value().find("\"mode\": 4") != std::string::npos);
    CHECK(json.value().find("\"pan\": 64") != std::string::npos);
    CHECK(json.value().find("\"pitch_bend_min\"") != std::string::npos);
    CHECK(json.value().find("\"tone_count\": 0") == std::string::npos);
    CHECK(json.value().find("\"sample_count\": 28") != std::string::npos);
    CHECK(json.value().find("4096") != std::string::npos);
    CHECK(json.value().find("\"pcm\"") == std::string::npos);
    request.full_pcm = true;
    json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\"pcm\"") != std::string::npos);

    request.body.clear();
    request.type = "vb";
    json = oscilline::inspect_to_json(request);
    CHECK_FALSE(json);
}

TEST_CASE("fsl walks sections from the header counts") {
    Buf file;
    file.u32(1);
    file.u32(1);
    file.u32(1);
    file.u32(1);
    file.u32(1);
    file.u32(1);
    file.u32(0);
    file.u32(0);
    file.u32(0);
    file.u32(2);
    file.u32(0);
    file.u32(1);
    file.u32(1);
    file.i32(-5);
    file.i32(1000);
    for (int i = 0; i < 10; ++i) {
        file.i32((i + 1) * 10);
    }
    file.u32(1);
    file.i32(10);
    file.i32(20);
    file.i32(30);
    file.i32(40);
    file.i32(50);
    file.u32(1);
    file.i32(3);
    file.i32(4);
    file.i32(1);
    file.i32(2);
    file.u32(1);
    file.i32(1);
    file.i32(2);
    file.i32(3);
    file.i32(4);
    file.i32(5);
    file.i32(6);
    file.u32(8);
    file.u32(9);
    file.u32(10);
    file.u8(0xAB);
    file.u8(0xCD);

    auto course = oscilline::parse_fsl(file.bytes);
    REQUIRE(course);
    CHECK(course.value().fixed_pattern_offset == 0x24);
    CHECK(course.value().distribution_pattern_offset == 0x30);
    CHECK(course.value().control_segment_offset == 0x64);
    CHECK(course.value().pattern_segment_offset == 0x7C);
    CHECK(course.value().event_segment_offset == 0x90);
    CHECK(course.value().track_index_offset == 0xAC);
    REQUIRE(course.value().fixed_patterns.size() == 1);
    CHECK(course.value().fixed_patterns[0].obstacles.size() == 2);
    CHECK(course.value().fixed_patterns[0].obstacles[1] == 1);
    REQUIRE(course.value().distribution_patterns.size() == 1);
    CHECK(course.value().distribution_patterns[0].random_seed == -5);
    CHECK(course.value().distribution_patterns[0].max_prob == 1000);
    CHECK(course.value().distribution_patterns[0].obstacle_prob[9] == 100);
    CHECK(course.value().control_tracks[0].segments[0].base_speed == 20);
    CHECK(course.value().pattern_tracks[0].segments[0].pattern_type == 1);
    CHECK(course.value().pattern_tracks[0].segments[0].index == 2);
    CHECK(course.value().event_tracks[0].segments[0].camera_event == 6);
    CHECK(course.value().track_index[0].control_segment_index == 8);
    CHECK(course.value().track_index[0].event_segment_index == 10);
    CHECK(course.value().trailing.size() == 2);

    oscilline::InspectRequest request;
    request.type = "fsl";
    request.bytes = file.bytes;
    auto json = oscilline::inspect_to_json(request);
    REQUIRE(json);
    CHECK(json.value().find("\"fixed_pattern_offset\": 36") != std::string::npos);
}

TEST_CASE("random buffers do not crash the parsers") {
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> bytes(0, 255);
    fuzz_parsers({});
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::vector<std::uint8_t> junk(static_cast<std::size_t>(attempt * 17));
        for (std::uint8_t& value : junk) {
            value = static_cast<std::uint8_t>(bytes(rng));
        }
        fuzz_parsers(junk);
    }
}

TEST_CASE("inspect cli dumps a pak and refuses a vb without a vh") {
    const auto file = sample_pak();
    const auto directory = std::filesystem::temp_directory_path() / "oscilline-inspect-test";
    remove_tree(directory);
    std::filesystem::create_directories(directory);
    const auto pak_path = directory / "sample.pak";
    {
        std::ofstream out(pak_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.bytes.data()),
                  static_cast<std::streamsize>(file.bytes.size()));
    }
    const auto listing = directory / "out.json";
    const std::string command =
        quote_arg(inspect_tool()) + " " + quote_arg(pak_path) + " > " + quote_arg(listing);
    REQUIRE(run_shell(command) == 0);
    std::stringstream buffer;
    {
        std::ifstream listed(listing);
        buffer << listed.rdbuf();
    }
    CHECK(buffer.str().find("\"name\": \"A.TIM\"") != std::string::npos);

    const auto body_path = directory / "tone.vb";
    {
        std::ofstream out(body_path, std::ios::binary);
        out << "0123456789abcdef";
    }
    const std::string refused = quote_arg(inspect_tool()) + " " + quote_arg(body_path) + " 2> " +
                                quote_arg(directory / "err.txt");
    CHECK(run_shell(refused) != 0);
    remove_tree(directory);
}
