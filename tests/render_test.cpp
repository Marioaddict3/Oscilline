// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Strokes, projection, the step clock, and wireframe colors.

#include "oscilline/anm.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "oscilline/render/clock.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"
#include "oscilline/tmd.hpp"
#include "support/iso_builder.hpp"

#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST_CASE("logical viewport width follows the selected aspect ratio") {
    using oscilline::ViewportAspect;
    CHECK(oscilline::logical_width_for_aspect(ViewportAspect::FourThree) == 640);
    CHECK(oscilline::logical_width_for_aspect(ViewportAspect::SixteenNine) == 853);
    CHECK(oscilline::logical_width_for_aspect(ViewportAspect::Unrestricted, 1920, 1080) == 853);
    CHECK(oscilline::logical_width_for_aspect(ViewportAspect::Unrestricted, 1920, 1200) == 768);
    CHECK(oscilline::logical_width_for_aspect(ViewportAspect::Unrestricted) == 640);
}

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

Buf line_model(std::int16_t x0,
               std::int16_t y0,
               std::int16_t z0,
               std::int16_t x1,
               std::int16_t y1,
               std::int16_t z1,
               std::int32_t scale) {
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
    file.i32(scale);
    file.i16(x0);
    file.i16(y0);
    file.i16(z0);
    file.u16(0);
    file.i16(x1);
    file.i16(y1);
    file.i16(z1);
    file.u16(0);
    file.u8(3);
    file.u8(2);
    file.u8(1);
    file.u8(0x40);
    file.u32(0x00FFFFFF);
    file.u16(0);
    file.u16(1);
    return file;
}

Buf triangle_model() {
    Buf file;
    file.u32(0x41);
    file.u32(0);
    file.u32(1);
    file.u32(0x1C);
    file.u32(3);
    file.u32(0);
    file.u32(0);
    file.u32(0x34);
    file.u32(1);
    file.i32(0);
    file.i16(0);
    file.i16(0);
    file.i16(0);
    file.u16(0);
    file.i16(10);
    file.i16(0);
    file.i16(0);
    file.u16(0);
    file.i16(0);
    file.i16(10);
    file.i16(0);
    file.u16(0);
    file.u8(4);
    file.u8(3);
    file.u8(1);
    file.u8(0x20);
    file.u32(0x00FFFFFF);
    file.u16(0);
    file.u16(1);
    file.u16(2);
    file.u16(0);
    return file;
}

void push_key(Buf& file, std::uint8_t object, std::int16_t position_x) {
    file.u8(object);
    file.u8(0x04);
    file.i16(position_x);
    file.i16(0);
    file.i16(0);
}

Buf position_animation(std::int16_t first, std::int16_t second) {
    Buf file;
    file.u16(0x8000);
    file.u16(30);
    file.u16(2);
    file.u16(6);
    file.u16(10);
    file.u16(14);
    push_key(file, 0, first);
    push_key(file, 0, second);
    return file;
}

oscilline::Camera ortho_camera() {
    oscilline::Camera camera;
    camera.mode = oscilline::Camera::Mode::Orthographic;
    camera.width = 100;
    camera.height = 100;
    camera.ortho_pixels_per_unit = 1;
    return camera;
}

} // namespace

TEST_CASE("thick strokes add a transparent feather and skip empty segments") {
    oscilline::Segment segment;
    segment.x0 = 0;
    segment.y0 = 0;
    segment.x1 = 10;
    segment.y1 = 0;
    oscilline::TriangleList solid;
    oscilline::append_strokes(solid, std::span<const oscilline::Segment>(&segment, 1), {2.f, 0.f});
    CHECK(solid.vertices.size() == 6);

    oscilline::TriangleList soft;
    oscilline::append_strokes(soft, std::span<const oscilline::Segment>(&segment, 1), {2.f, 1.f});
    CHECK(soft.vertices.size() == 18);
    bool opaque = false;
    bool clear = false;
    for (const oscilline::Vertex& vertex : soft.vertices) {
        CHECK(vertex.x >= -0.01f);
        CHECK(vertex.x <= 10.01f);
        CHECK(vertex.y >= -2.01f);
        CHECK(vertex.y <= 2.01f);
        opaque = opaque || vertex.a > 0.9f;
        clear = clear || vertex.a < 0.1f;
    }
    CHECK(opaque);
    CHECK(clear);

    segment.x1 = 0;
    oscilline::TriangleList empty;
    oscilline::append_strokes(empty, std::span<const oscilline::Segment>(&segment, 1));
    CHECK(empty.vertices.empty());
}

TEST_CASE("strokes are ordered far to near") {
    oscilline::Segment far;
    far.x0 = 0;
    far.y0 = 0;
    far.x1 = 4;
    far.y1 = 0;
    far.depth = 9;
    far.color.r = 0.25f;
    oscilline::Segment near = far;
    near.y0 = 3;
    near.y1 = 3;
    near.depth = 1;
    near.color.r = 0.75f;
    const oscilline::Segment pair[] = {near, far};
    oscilline::TriangleList triangles;
    oscilline::append_strokes(triangles, pair, {2.f, 0.f});
    REQUIRE(triangles.vertices.size() == 12);
    CHECK(triangles.vertices.front().r == doctest::Approx(0.25f));
    CHECK(triangles.vertices[6].r == doctest::Approx(0.75f));
}

TEST_CASE("orthographic projection maps model units onto the logical view") {
    oscilline::ModelSegment model;
    model.a = {0, 0, 0};
    model.b = {10, -4, 2};
    const auto screen = oscilline::project_segments(
        std::span<const oscilline::ModelSegment>(&model, 1), ortho_camera());
    REQUIRE(screen.size() == 1);
    CHECK(screen[0].x0 == doctest::Approx(50));
    CHECK(screen[0].y0 == doctest::Approx(50));
    CHECK(screen[0].x1 == doctest::Approx(60));
    CHECK(screen[0].y1 == doctest::Approx(54));
    CHECK(screen[0].depth == doctest::Approx(-1));
}

TEST_CASE("perspective drops a segment that sits on the camera") {
    oscilline::Camera camera;
    camera.mode = oscilline::Camera::Mode::Perspective;
    camera.eye_z = 10;
    camera.near_plane = 1;
    oscilline::ModelSegment behind;
    behind.a = {0, 0, 10};
    behind.b = {1, 0, 10};
    CHECK(oscilline::project_segments(std::span<const oscilline::ModelSegment>(&behind, 1), camera)
              .empty());
    oscilline::ModelSegment ahead;
    ahead.a = {0, 0, 0};
    ahead.b = {0, 1, 0};
    CHECK(oscilline::project_segments(std::span<const oscilline::ModelSegment>(&ahead, 1), camera)
              .size() == 1);
}

TEST_CASE("tmd lines and polygon edges become wireframe segments") {
    auto parsed = oscilline::parse_tmd(line_model(0, 0, 0, 10, 0, 0, 0).bytes);
    REQUIRE(parsed);
    const auto lines = oscilline::tmd_wireframe(parsed.value());
    REQUIRE(lines.size() == 1);
    const auto screen = oscilline::project_segments(lines, ortho_camera());
    REQUIRE(screen.size() == 1);
    CHECK(screen[0].x0 == doctest::Approx(50));
    CHECK(screen[0].x1 == doctest::Approx(60));
    CHECK(screen[0].y0 == doctest::Approx(50));

    auto shifted = oscilline::parse_tmd(line_model(0, 0, 0, 4, 0, 0, 1).bytes);
    REQUIRE(shifted);
    const auto scaled =
        oscilline::project_segments(oscilline::tmd_wireframe(shifted.value()), ortho_camera());
    REQUIRE(scaled.size() == 1);
    CHECK(scaled[0].x1 == doctest::Approx(58));

    auto face = oscilline::parse_tmd(triangle_model().bytes);
    REQUIRE(face);
    CHECK(oscilline::tmd_wireframe(face.value()).size() == 3);

    oscilline::Pose hidden;
    hidden.visible = false;
    const oscilline::Pose poses[] = {hidden};
    CHECK(oscilline::tmd_wireframe(parsed.value(), poses).empty());
}

TEST_CASE("head occluder fills can be omitted from the figure mesh") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    // The packet's top byte is the primitive code (0x21, a flat triangle).
    oscilline::TmdPrimitive head;
    head.kind = oscilline::PrimitiveKind::Polygon;
    head.color = 0x21010000u;
    head.vertex_indices = {0, 1, 2};
    oscilline::TmdPrimitive eye = head;
    eye.color = 0x21000000u;
    oscilline::TmdPrimitive visible = head;
    visible.color = 0x210000FFu;
    object.primitives = {head, eye, visible};
    model.objects.push_back(object);

    oscilline::WireframeOptions options;
    options.packet_color = true;
    CHECK(oscilline::tmd_figure_mesh(model, {}, options).triangles.size() == 3);
    options.skip_black_occluders = true;
    const oscilline::FigureMesh mesh = oscilline::tmd_figure_mesh(model, {}, options);
    REQUIRE(mesh.triangles.size() == 2);
    // The eye fill stays black and the red fill stays.
    CHECK(mesh.triangles[0].color.r < 0.05f);
    CHECK(mesh.triangles[1].color.r > 0.95f);
}

TEST_CASE("anm poses hide unkeyed objects and interpolate position") {
    auto animation = oscilline::parse_anm(position_animation(0, 10).bytes);
    REQUIRE(animation);
    const auto first = oscilline::poses_for_frame(animation.value(), 2, 0, 0.f, false);
    REQUIRE(first.size() == 2);
    CHECK(first[0].visible);
    CHECK(first[0].position_x == doctest::Approx(0));
    CHECK_FALSE(first[1].visible);

    const auto mid = oscilline::poses_for_frame(animation.value(), 1, 0, 0.5f, true);
    REQUIRE(mid.size() == 1);
    CHECK(mid[0].position_x == doctest::Approx(5));

    Buf spin;
    spin.u16(0x8000);
    spin.u16(30);
    spin.u16(2);
    spin.u16(6);
    spin.u16(10);
    spin.u16(14);
    spin.u8(0);
    spin.u8(0x01);
    spin.i16(0);
    spin.i16(0);
    spin.i16(0);
    spin.u8(0);
    spin.u8(0x01);
    spin.i16(0);
    spin.i16(0);
    spin.i16(4096);
    auto turned = oscilline::parse_anm(spin.bytes);
    REQUIRE(turned);
    const auto blended = oscilline::poses_for_frame(turned.value(), 1, 0, 0.5f, true);
    CHECK(blended[0].rotation_z == doctest::Approx(0).epsilon(0.001));

    auto model = oscilline::parse_tmd(line_model(0, 0, 0, 1, 0, 0, 0).bytes);
    REQUIRE(model);
    const auto posed = oscilline::tmd_wireframe(model.value(), first);
    REQUIRE(posed.size() == 1);
    oscilline::Pose yaw;
    yaw.visible = true;
    yaw.rotation_z = std::numbers::pi_v<float> * 0.5f;
    const oscilline::Pose turn[] = {yaw};
    const auto rotated = oscilline::tmd_wireframe(model.value(), turn);
    REQUIRE(rotated.size() == 1);
    CHECK(rotated[0].b.x == doctest::Approx(0).epsilon(0.001));
    CHECK(rotated[0].b.y == doctest::Approx(1).epsilon(0.001));
}

TEST_CASE("fixed step latches the first sample and reports the leftover fraction") {
    oscilline::FixedStep clock(0.1);
    CHECK(clock.advance(10.0) == 0);
    CHECK(clock.alpha() == doctest::Approx(0));
    CHECK(clock.advance(10.25) == 2);
    CHECK(clock.alpha() == doctest::Approx(0.5));
    CHECK(clock.advance(12.0, 4) == 4);
    CHECK(clock.alpha() == doctest::Approx(0));
}

TEST_CASE("archive lookup is case-insensitive and prefers the english pak") {
    oscilline::PakArchive archive;
    oscilline::PakEntry model;
    model.name = "CHARA\\Model.TMD";
    oscilline::PakEntry clip;
    clip.name = "chara/walk.anm";
    oscilline::PakEntry note;
    note.name = "readme.txt";
    archive.entries.push_back(model);
    archive.entries.push_back(clip);
    archive.entries.push_back(note);

    const oscilline::PakEntry* found = oscilline::find_entry(archive, "chara/model.tmd");
    REQUIRE(found != nullptr);
    CHECK(found->name == "CHARA\\Model.TMD");
    const oscilline::ModelList listed = oscilline::list_models(archive);
    REQUIRE(listed.tmd.size() == 1);
    REQUIRE(listed.anm.size() == 1);
    CHECK(listed.anm[0] == "chara/walk.anm");

    using oscilline::testutil::bytes_from;
    using oscilline::testutil::IsoNode;
    Buf pak;
    pak.u32(1);
    pak.u32(8);
    pak.text("A.TMD");
    pak.u8(0);
    pak.u8(0);
    pak.u8(0);
    pak.u32(1);
    pak.u8(0x41);

    IsoNode root;
    root.directory = true;
    IsoNode game;
    game.name = "GAME";
    game.directory = true;
    game.xa_attributes = 0x8D55;
    IsoNode file;
    file.name = "02_FILES.PAK";
    file.data = pak.bytes;
    file.xa_attributes = 0x0D55;
    game.children.push_back(std::move(file));
    root.children.push_back(std::move(game));

    auto image =
        oscilline::DiscImage::open_iso_bytes(oscilline::testutil::build_iso(root, "OSCILLINE"));
    REQUIRE(image);
    auto volume = oscilline::IsoVolume::read(image.value());
    REQUIRE(volume);
    auto loaded = oscilline::load_language_pak(volume.value());
    REQUIRE(loaded);
    CHECK(loaded.value().entries.size() == 1);
    CHECK(loaded.value().entries[0].name == "A.TMD");

    auto missing = oscilline::load_language_pak(volume.value(), "GAME/NOPE.PAK");
    CHECK_FALSE(missing);
    CHECK(missing.error().find("NOPE.PAK") != std::string::npos);
}

TEST_CASE("degenerate triangles are dropped before they would be drawn") {
    using namespace oscilline;
    TriangleList list;
    const auto push = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        Vertex a;
        a.x = x0;
        a.y = y0;
        Vertex b;
        b.x = x1;
        b.y = y1;
        Vertex c;
        c.x = x2;
        c.y = y2;
        list.vertices.push_back(a);
        list.vertices.push_back(b);
        list.vertices.push_back(c);
    };
    push(0.f, 0.f, 10.f, 0.f, 0.f, 10.f);
    push(0.f, 0.f, 5.f, 0.f, 9.f, 0.f);
    push(1.f, 1.f, 1.f, 1.f, 1.f, 1.f);
    push(-80.f, -80.f, -40.f, -80.f, -80.f, -20.f);
    push(-10.f, 10.f, 20.f, 10.f, 20.f, 40.f);
    Vertex broken;
    broken.x = std::numeric_limits<float>::quiet_NaN();
    broken.y = 0.f;
    list.vertices.push_back(broken);
    list.vertices.push_back(Vertex{});
    list.vertices.push_back(Vertex{});
    list.vertices.push_back(Vertex{});
    CHECK_FALSE(triangle_degenerate(list.vertices[0], list.vertices[1], list.vertices[2]));
    CHECK(triangle_degenerate(list.vertices[3], list.vertices[4], list.vertices[5]));
    const TriangleList kept = drop_degenerate_triangles(list, 640.f, 480.f);
    // The in-view triangle stays. The one that crosses x = 0 is clipped to a quad.
    REQUIRE(kept.vertices.size() == 9);
    CHECK(kept.vertices[1].x == doctest::Approx(10.f));
    CHECK(kept.vertices[2].y == doctest::Approx(10.f));
    for (const Vertex& vertex : kept.vertices) {
        CHECK(vertex.x >= 0.f);
        CHECK(vertex.x <= 640.f);
        CHECK(vertex.y >= 0.f);
        CHECK(vertex.y <= 480.f);
    }
    CHECK(drop_degenerate_triangles(TriangleList{}).vertices.empty());
}

TEST_CASE("psx model space maps to the side and front views without mirroring") {
    using oscilline::ModelVertex;
    using oscilline::ModelView;
    const ModelVertex right{1.f, 0.f, 0.f};
    const ModelVertex down{0.f, 1.f, 0.f};
    const ModelVertex away{0.f, 0.f, 1.f};
    const auto side_x = oscilline::to_view_space(right, ModelView::Side);
    const auto side_y = oscilline::to_view_space(down, ModelView::Side);
    const auto side_z = oscilline::to_view_space(away, ModelView::Side);
    CHECK(side_x.x == doctest::Approx(0.f));
    CHECK(side_x.y == doctest::Approx(0.f));
    CHECK(side_x.z == doctest::Approx(1.f));
    CHECK(side_y.x == doctest::Approx(0.f));
    CHECK(side_y.y == doctest::Approx(-1.f));
    CHECK(side_y.z == doctest::Approx(0.f));
    CHECK(side_z.x == doctest::Approx(1.f));
    CHECK(side_z.y == doctest::Approx(0.f));
    CHECK(side_z.z == doctest::Approx(0.f));
    const auto front_x = oscilline::to_view_space(right, ModelView::Front);
    const auto front_y = oscilline::to_view_space(down, ModelView::Front);
    const auto front_z = oscilline::to_view_space(away, ModelView::Front);
    CHECK(front_x.x == doctest::Approx(1.f));
    CHECK(front_x.y == doctest::Approx(0.f));
    CHECK(front_x.z == doctest::Approx(0.f));
    CHECK(front_y.y == doctest::Approx(-1.f));
    CHECK(front_z.z == doctest::Approx(-1.f));
    for (ModelView view : {ModelView::Side, ModelView::Front}) {
        const auto ex = oscilline::to_view_space(right, view);
        const auto ey = oscilline::to_view_space(down, view);
        const auto ez = oscilline::to_view_space(away, view);
        const float cx = ex.y * ey.z - ex.z * ey.y;
        const float cy = ex.z * ey.x - ex.x * ey.z;
        const float cz = ex.x * ey.y - ex.y * ey.x;
        CHECK(cx == doctest::Approx(ez.x));
        CHECK(cy == doctest::Approx(ez.y));
        CHECK(cz == doctest::Approx(ez.z));
    }
}

TEST_CASE("framing camera keeps a tall figure inside the view") {
    std::vector<oscilline::ModelSegment> figure{
        {{0.f, 0.f, -40.f}, {0.f, -600.f, 40.f}},
        {{0.f, -300.f, -40.f}, {0.f, -300.f, 40.f}},
    };
    const auto view = oscilline::to_view_space(figure, oscilline::ModelView::Side);
    oscilline::ModelBounds bounds;
    oscilline::extend_bounds(bounds, view);
    const auto camera = oscilline::framing_camera(bounds);
    const auto screen = oscilline::project_segments(view, camera);
    REQUIRE(screen.size() == figure.size());
    for (const auto& segment : screen) {
        for (float y : {segment.y0, segment.y1}) {
            CHECK(y >= 0.f);
            CHECK(y <= camera.height);
        }
        for (float x : {segment.x0, segment.x1}) {
            CHECK(x >= 0.f);
            CHECK(x <= camera.width);
        }
    }
    CHECK(screen[0].y1 < screen[0].y0);

    oscilline::ModelBounds empty;
    oscilline::ModelSegment broken;
    broken.a = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
    broken.b = {1.f, 2.f, 3.f};
    oscilline::extend_bounds(empty, std::span<const oscilline::ModelSegment>(&broken, 1));
    CHECK_FALSE(empty.empty);
    CHECK(empty.min.x == doctest::Approx(1.f));
    CHECK(oscilline::framing_camera(oscilline::ModelBounds{}).eye_z == doctest::Approx(512.f));
}

TEST_CASE("pose order is x then y then z and flags 5 keeps unit scale") {
    auto model = oscilline::parse_tmd(line_model(0, 1, 0, 0, 1, 0, 0).bytes);
    REQUIRE(model);
    Buf spin;
    spin.u16(0x8000);
    spin.u16(30);
    spin.u16(1);
    spin.u16(5);
    spin.u16(9);
    spin.u8(0);
    spin.u8(0x01);
    spin.i16(1024);
    spin.i16(1024);
    spin.i16(0);
    auto turned = oscilline::parse_anm(spin.bytes);
    REQUIRE(turned);
    const auto poses = oscilline::poses_for_frame(turned.value(), 1, 0, 0.f, false);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].scale_x == doctest::Approx(1.f));
    const auto lines = oscilline::tmd_wireframe(model.value(), poses);
    REQUIRE(lines.size() == 1);
    // (0, 1, 0) through Rx 90 then Ry 90 lands on +X. Z-first would not.
    CHECK(lines[0].a.x == doctest::Approx(1.f).epsilon(0.001));
    CHECK(lines[0].a.y == doctest::Approx(0.f).epsilon(0.001));
    CHECK(lines[0].a.z == doctest::Approx(0.f).epsilon(0.001));

    Buf shift;
    shift.u16(0x8000);
    shift.u16(30);
    shift.u16(1);
    shift.u16(5);
    shift.u16(12);
    shift.u8(0);
    shift.u8(0x05);
    shift.i16(0);
    shift.i16(0);
    shift.i16(0);
    shift.i16(4);
    shift.i16(0);
    shift.i16(0);
    auto moved = oscilline::parse_anm(shift.bytes);
    REQUIRE(moved);
    const auto placed = oscilline::poses_for_frame(moved.value(), 1, 0, 0.f, false);
    CHECK(placed[0].scale_x == doctest::Approx(1.f));
    CHECK(placed[0].scale_y == doctest::Approx(1.f));
    CHECK(placed[0].scale_z == doctest::Approx(1.f));
    CHECK(placed[0].position_x == doctest::Approx(4.f));
    auto unit = oscilline::parse_tmd(line_model(1, 0, 0, 1, 0, 0, 0).bytes);
    REQUIRE(unit);
    const auto shifted = oscilline::tmd_wireframe(unit.value(), placed);
    REQUIRE(shifted.size() == 1);
    CHECK(shifted[0].a.x == doctest::Approx(5.f));

    Buf flat;
    flat.u16(0x8000);
    flat.u16(30);
    flat.u16(1);
    flat.u16(5);
    flat.u16(15);
    flat.u8(0);
    flat.u8(0x07);
    flat.i16(0);
    flat.i16(0);
    flat.i16(0);
    flat.i16(4096);
    flat.i16(4096);
    flat.i16(0);
    flat.i16(0);
    flat.i16(0);
    flat.i16(0);
    auto scaled = oscilline::parse_anm(flat.bytes);
    REQUIRE(scaled);
    const auto scales = oscilline::poses_for_frame(scaled.value(), 1, 0, 0.f, false);
    CHECK(scales[0].scale_x == doctest::Approx(1.f));
    CHECK(scales[0].scale_z == doctest::Approx(0.f));
}

TEST_CASE("an unkeyed object is omitted from the posed wireframe") {
    oscilline::TmdModel model;
    model.objects.resize(2);
    for (oscilline::TmdObject& object : model.objects) {
        object.vertices.push_back({0, 0, 0});
        object.vertices.push_back({1, 0, 0});
        oscilline::TmdPrimitive line;
        line.kind = oscilline::PrimitiveKind::Line;
        line.vertex_indices = {0, 1};
        object.primitives.push_back(line);
    }
    oscilline::AnmFile animation;
    animation.unk1 = 30;
    animation.frame_count = 1;
    animation.frames.resize(1);
    oscilline::AnmKeyframe key;
    key.object_index = 0;
    key.flags = 0x04;
    key.has_position = true;
    animation.frames[0].keys.push_back(key);
    const auto poses = oscilline::poses_for_frame(animation, 2, 0, 0.f, false);
    CHECK(poses[0].visible);
    CHECK_FALSE(poses[1].visible);
    CHECK(oscilline::tmd_wireframe(model, poses).size() == 1);
}

TEST_CASE("anm playback rate uses the header field") {
    CHECK(oscilline::anm_playback_hz(60) == doctest::Approx(60.f));
    CHECK(oscilline::anm_playback_hz(7) == doctest::Approx(30.f));
}
