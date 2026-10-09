// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Course picture: ribbon, obstacles, figure, and the disc camera.

#include "course_draw.hpp"
#include "oscilline/course/camera.hpp"
#include "oscilline/course/jitter.hpp"
#include "oscilline/course/shapes.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

oscilline::AnmFile still_clip() {
    oscilline::AnmFile clip;
    clip.unk1 = 30;
    clip.frame_count = 1;
    clip.frames.resize(1);
    oscilline::AnmKeyframe key;
    key.object_index = 0;
    clip.frames[0].keys.push_back(key);
    return clip;
}

oscilline::TmdPrimitive line_prim(std::uint16_t a, std::uint16_t b, std::uint32_t color) {
    oscilline::TmdPrimitive primitive;
    primitive.kind = oscilline::PrimitiveKind::Line;
    primitive.vertex_indices = {a, b};
    primitive.color = color;
    return primitive;
}

oscilline::TmdPrimitive polygon_prim(std::initializer_list<std::uint16_t> indices,
                                     std::uint32_t color) {
    oscilline::TmdPrimitive primitive;
    primitive.kind = oscilline::PrimitiveKind::Polygon;
    primitive.vertex_indices.assign(indices.begin(), indices.end());
    primitive.color = color;
    return primitive;
}

oscilline::CharacterPlacement placement_of(float scale, float anchor_x, float ground_y) {
    oscilline::CharacterPlacement placement;
    placement.valid = true;
    placement.scale = scale;
    placement.anchor_x = anchor_x;
    placement.ground_y = ground_y;
    return placement;
}

oscilline::CourseFrame draw_at(std::int64_t time_ms, bool guides) {
    oscilline::CourseTimeline course;
    course.duration_ms = 60000;
    oscilline::PlayState play;
    oscilline::CourseView view;
    view.ribbon_guides = guides;
    return oscilline::draw_course(course, play, time_ms, view);
}

bool near_white(const oscilline::Vertex& vertex) {
    return vertex.r > 0.95f && vertex.g > 0.95f && vertex.b > 0.95f;
}

bool near_red(const oscilline::Vertex& vertex) {
    return vertex.r > 0.95f && vertex.g < 0.05f && vertex.b < 0.05f;
}

bool near_black(const oscilline::Vertex& vertex) {
    return vertex.r < 0.05f && vertex.g < 0.05f && vertex.b < 0.05f;
}

bool near_blue(const oscilline::Vertex& vertex) {
    return vertex.r < 0.05f && vertex.g < 0.05f && vertex.b > 0.95f;
}

float cross2(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

bool point_in_triangle(
    float px, float py, float x0, float y0, float x1, float y1, float x2, float y2) {
    const float c0 = cross2(x0, y0, x1, y1, px, py);
    const float c1 = cross2(x1, y1, x2, y2, px, py);
    const float c2 = cross2(x2, y2, x0, y0, px, py);
    const bool neg = c0 < 0.f || c1 < 0.f || c2 < 0.f;
    const bool pos = c0 > 0.f || c1 > 0.f || c2 > 0.f;
    return !(neg && pos);
}

oscilline::AncSample side_sample(float distance, float fov, float roll) {
    oscilline::AncSample sample;
    sample.eye_x = distance;
    sample.fov = fov;
    sample.roll = roll;
    return sample;
}

void segment_span(const std::vector<oscilline::Segment>& lines,
                  float& min_x,
                  float& max_x,
                  float& min_y,
                  float& max_y) {
    min_x = max_x = lines.front().x0;
    min_y = max_y = lines.front().y0;
    const auto take = [&](float x, float y) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    };
    for (const oscilline::Segment& line : lines) {
        take(line.x0, line.y0);
        take(line.x1, line.y1);
    }
}

} // namespace

TEST_CASE("course strokes are a 1 px core with a half-pixel feather") {
    CHECK(oscilline::kCourseStrokeWidth == doctest::Approx(1.f));
    CHECK(oscilline::kCourseStrokeFeather == doctest::Approx(0.5f));

    // The ribbon spans the screen and rests at 1 PS px of jitter, so each end is
    // measured against its own center line.
    const oscilline::CourseFrame frame = draw_at(0, false);
    int ribbon = 0;
    int feather = 0;
    for (const float end_x : {0.f, static_cast<float>(oscilline::kLogicalWidth)}) {
        std::vector<oscilline::Vertex> core;
        std::vector<oscilline::Vertex> halo;
        for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
            if (std::fabs(vertex.x - end_x) > 0.5f || std::fabs(vertex.y - 240.f) > 4.f ||
                !near_white(vertex)) {
                continue;
            }
            (vertex.a > 0.5f ? core : halo).push_back(vertex);
        }
        REQUIRE_FALSE(core.empty());
        float center = 0.f;
        for (const oscilline::Vertex& vertex : core) {
            center += vertex.y;
        }
        center /= static_cast<float>(core.size());
        CHECK(std::fabs(center - 240.f) <= 1.7f);
        for (const oscilline::Vertex& vertex : core) {
            CHECK(std::fabs(vertex.y - center) <= 0.51f);
            ++ribbon;
        }
        for (const oscilline::Vertex& vertex : halo) {
            const float dy = std::fabs(vertex.y - center);
            CHECK(dy >= 0.9f);
            CHECK(dy <= 1.01f);
            ++feather;
        }
    }
    CHECK(ribbon > 0);
    CHECK(feather > 0);

    bool hud_feather = false;
    for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
        if (vertex.y > 467.2f && vertex.a < 0.05f && vertex.x > 90.f && vertex.x < 180.f) {
            hud_feather = true;
        }
    }
    CHECK(hud_feather);
}

TEST_CASE("guide ticks sit on the perfect and good window edges") {
    oscilline::CourseTimeline course;
    course.duration_ms = 60000;
    oscilline::CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 2000;
    event.approach_ms = 2000;
    event.scroll_approach_ms = 2000;
    course.events.push_back(event);
    oscilline::PlayState play;
    oscilline::CourseView guides;
    guides.ribbon_guides = true;
    const oscilline::CourseFrame guided = oscilline::draw_course(course, play, 1000, guides);
    const oscilline::CourseFrame plain = oscilline::draw_course(course, play, 1000);
    const oscilline::ObstacleWindow window = oscilline::obstacle_window(event);
    constexpr float kWidth = 640.f;
    const float figure_x = kWidth * oscilline::kHitXFraction;
    const float spawn_x = kWidth * oscilline::kSpawnXFraction;
    const float close_x =
        oscilline::obstacle_screen_x(figure_x,
                                     spawn_x,
                                     oscilline::stage_scroll_approach_ms(event),
                                     static_cast<std::int32_t>(window.perfect_close),
                                     1000);
    const float center_x = oscilline::obstacle_screen_x(
        figure_x, spawn_x, oscilline::stage_scroll_approach_ms(event), event.hit_ms, 1000);
    const float leading =
        oscilline::obstacle_leading_x(event.obstacle, kWidth * oscilline::kLoopWidthFraction);
    const float px_per_ms =
        (spawn_x - figure_x) / static_cast<float>(oscilline::stage_scroll_approach_ms(event));
    const float bias_px = static_cast<float>(oscilline::kJudgmentLateBiasMs) * px_per_ms;
    // The block's front edge sits on the origin, so perfect closes at the hit
    // plus the late bias. Good continues past that close, into the obstacle.
    CHECK(leading == doctest::Approx(0.f).epsilon(0.01));
    CHECK(close_x == doctest::Approx(center_x + leading + bias_px).epsilon(0.02));
    CHECK(close_x > center_x);

    const auto colored = [](const oscilline::CourseFrame& frame, float x, bool want_yellow) {
        for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
            if (std::fabs(vertex.x - x) > 2.f) {
                continue;
            }
            const bool yellow = vertex.r > 0.9f && vertex.g > 0.7f && vertex.b < 0.4f;
            const bool blue = vertex.b > 0.9f && vertex.g > 0.6f && vertex.r < 0.6f;
            if (want_yellow ? yellow : blue) {
                return true;
            }
        }
        return false;
    };
    const float good_x = oscilline::obstacle_screen_x(figure_x,
                                                      spawn_x,
                                                      oscilline::stage_scroll_approach_ms(event),
                                                      static_cast<std::int32_t>(window.good_open),
                                                      1000);
    const float late_good_x =
        oscilline::obstacle_screen_x(figure_x,
                                     spawn_x,
                                     oscilline::stage_scroll_approach_ms(event),
                                     static_cast<std::int32_t>(window.good_close),
                                     1000);
    CHECK(colored(guided, close_x, true));
    CHECK(colored(guided, good_x, false));
    CHECK(colored(guided, late_good_x, false));
    CHECK(late_good_x > close_x);
    CHECK_FALSE(colored(plain, close_x, true));
}

TEST_CASE("ribbon ticks and the hit marker need --ribbon-guides") {
    const oscilline::CourseFrame plain = draw_at(0, false);
    const oscilline::CourseFrame guided = draw_at(0, true);
    const auto tick = [](const oscilline::CourseFrame& frame) {
        for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
            if (vertex.x > 120.f && vertex.x < 560.f && std::fabs(vertex.y - 240.f) > 4.f &&
                std::fabs(vertex.y - 240.f) < 16.f) {
                return true;
            }
        }
        return false;
    };
    const auto hit = [](const oscilline::CourseFrame& frame) {
        for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
            if (std::fabs(vertex.x - 76.8f) < 2.f && vertex.y > 260.f) {
                return true;
            }
        }
        return false;
    };
    CHECK_FALSE(tick(plain));
    CHECK_FALSE(hit(plain));
    CHECK(tick(guided));
    CHECK(hit(guided));
}

TEST_CASE("the ribbon leaves out the piece under an obstacle's own outline") {
    oscilline::CourseTimeline course;
    course.duration_ms = 60000;
    course.events.push_back({0, 2000, 2000, 2000});
    const oscilline::PlayState play;
    const oscilline::CourseFrame frame = oscilline::draw_course(course, play, 1000);
    const std::vector<oscilline::Vertex>& vertices = frame.triangles.vertices;
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (const oscilline::Vertex& vertex : vertices) {
        if (vertex.x > 160.f && std::fabs(vertex.y - 240.f) > 8.f && near_white(vertex)) {
            lo = std::min(lo, vertex.x);
            hi = std::max(hi, vertex.x);
        }
    }
    REQUIRE(hi - lo > 10.f);
    int flat_pieces = 0;
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        float x0 = 1.0e9f;
        float x1 = -1.0e9f;
        bool flat = true;
        for (std::size_t k = i; k < i + 3; ++k) {
            x0 = std::min(x0, vertices[k].x);
            x1 = std::max(x1, vertices[k].x);
            flat = flat && std::fabs(vertices[k].y - 240.f) < 3.f && near_white(vertices[k]);
        }
        if (!flat || x1 - x0 < 20.f) {
            continue;
        }
        ++flat_pieces;
        const float mid = (x0 + x1) * 0.5f;
        CHECK_FALSE((mid > lo && mid < hi));
    }
    CHECK(flat_pieces > 0);
}

TEST_CASE("an eye outline is drawn over its own black fill when tilted") {
    // An eye: a black fill at model x 0 and an outline that, tilted, sits
    // deeper (smaller x is farther in the side view).
    const auto eye_model = [](int outline_lines) {
        oscilline::TmdModel model;
        oscilline::TmdObject eye;
        eye.vertices = {{0, -10, -10}, {0, 10, -10}, {0, 0, 10}};
        eye.primitives.push_back(polygon_prim({0, 1, 2}, 0x21000000u));
        for (int i = 0; i < outline_lines; ++i) {
            const auto first = static_cast<std::uint16_t>(eye.vertices.size());
            const auto y = static_cast<std::int16_t>(-12 + i * 3);
            eye.vertices.push_back({-5, y, -12});
            eye.vertices.push_back({-5, y, 12});
            eye.primitives.push_back(
                line_prim(first, static_cast<std::uint16_t>(first + 1), 0x00FFFFFFu));
        }
        model.objects.push_back(eye);
        return model;
    };
    const oscilline::AnmFile clip = still_clip();
    const auto paint = [&](const oscilline::TmdModel& model) {
        oscilline::DiscFigurePose pose;
        pose.model = &model;
        pose.clip = &clip;
        pose.loop = false;
        pose.placement = placement_of(1.f, 0.f, 0.f);
        std::vector<oscilline::Segment> lines;
        std::vector<oscilline::FilledTriangle> fills;
        CHECK(oscilline::paint_disc_figure(lines, pose, 200.f, 240.f, &fills));
        REQUIRE(fills.size() == 1);
        float deepest_line = -1.e9f;
        for (const oscilline::Segment& line : lines) {
            deepest_line = std::max(deepest_line, line.depth);
        }
        return std::pair<float, float>{deepest_line, fills[0].depth};
    };
    // Eight lines make an eye outline: every line paints after (nearer than) the fill.
    const auto eye = paint(eye_model(oscilline::kEyeOutlineLines));
    CHECK(eye.first < eye.second);
    // One line is not an eye; depth order alone puts the deeper line under the fill.
    const auto plain = paint(eye_model(1));
    CHECK(plain.first > plain.second);
}

TEST_CASE("the projected figure keeps its proportions at 4:3 and 16:9") {
    // A square outline in the side plane (model z across, y up).
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, -20}, {0, 0, 20}, {0, -40, 20}, {0, -40, -20}};
    object.primitives.push_back(line_prim(0, 1, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(1, 2, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(2, 3, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(3, 0, 0x00FFFFFFu));
    model.objects.push_back(object);
    const oscilline::AnmFile clip = still_clip();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(1.f, 0.f, 0.f);
    // The play camera: on +X, looking along -X.
    oscilline::AncSample camera;
    camera.eye_x = 5204.f;
    camera.eye_y = -700.f;
    camera.eye_z = 2061.f;
    camera.target_y = -700.f;
    camera.target_z = 2061.f;
    camera.fov = 499.f;
    const auto aspect_at = [&](int width) {
        const int saved = oscilline::logical_width();
        oscilline::set_logical_width(width);
        std::vector<oscilline::Segment> lines;
        CHECK(oscilline::paint_disc_figure(lines, pose, 0.f, 0.f, nullptr, &camera));
        oscilline::set_logical_width(saved);
        float x0 = 1.e9f;
        float x1 = -1.e9f;
        float y0 = 1.e9f;
        float y1 = -1.e9f;
        for (const oscilline::Segment& line : lines) {
            x0 = std::min({x0, line.x0, line.x1});
            x1 = std::max({x1, line.x0, line.x1});
            y0 = std::min({y0, line.y0, line.y1});
            y1 = std::max({y1, line.y0, line.y1});
        }
        return (x1 - x0) / (y1 - y0);
    };
    const float four_three = aspect_at(640);
    const float sixteen_nine = aspect_at(853);
    CHECK(four_three == doctest::Approx(sixteen_nine).epsilon(0.01));
    // Square pixels: a square outline stays roughly square on screen.
    CHECK(four_three == doctest::Approx(1.f).epsilon(0.1));
}

TEST_CASE("a line on a surface is drawn over the fill it sits on") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    // A black face at model x 1, an outline just behind it at x 0, and a
    // far-side line 30 units behind. Larger model x is nearer in the side view.
    object.vertices = {
        {1, 30, -35},
        {1, -40, 0},
        {1, 30, 35},
        {0, 0, -10},
        {0, 0, 10},
        {-30, 0, -10},
        {-30, 0, 10},
    };
    object.primitives.push_back(polygon_prim({0, 1, 2}, 0x00000000u));
    object.primitives.push_back(line_prim(3, 4, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(5, 6, 0x00FFFFFFu));
    model.objects.push_back(object);

    const oscilline::AnmFile clip = still_clip();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(1.f, 0.f, 0.f);
    std::vector<oscilline::Segment> lines;
    std::vector<oscilline::FilledTriangle> fills;
    CHECK(oscilline::paint_disc_figure(lines, pose, 200.f, 240.f, &fills));
    REQUIRE(lines.size() == 2);
    REQUIRE(fills.size() == 1);
    const oscilline::Segment& surface = lines[0].depth < lines[1].depth ? lines[0] : lines[1];
    const oscilline::Segment& far_side = lines[0].depth < lines[1].depth ? lines[1] : lines[0];
    // Smaller depth is nearer and is painted later.
    CHECK(surface.depth < fills[0].depth);
    CHECK(far_side.depth > fills[0].depth);
}

TEST_CASE("a black occluder is a fill drawn over the line it covers") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {
        {0, 0, -20},
        {0, 0, 20},
        {25, 30, -30},
        {25, -40, 0},
        {25, 30, 30},
        {-15, 0, 0},
        {-15, -10, 0},
        {-15, 0, 10},
    };
    object.primitives.push_back(line_prim(0, 1, 0x00FFFFFFu));
    object.primitives.push_back(polygon_prim({2, 3, 4}, 0x00000000u));
    object.primitives.push_back(polygon_prim({5, 6, 7}, 0x00FF0000u));
    model.objects.push_back(object);

    const oscilline::FigureMesh mesh = oscilline::tmd_figure_mesh(model);
    CHECK(mesh.lines.size() == 1);
    CHECK(mesh.triangles.size() == 2);
    CHECK(oscilline::tmd_wireframe(model).size() == 1 + 3 + 3);

    const oscilline::AnmFile clip = still_clip();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(1.f, 0.f, 0.f);
    std::vector<oscilline::Segment> lines;
    std::vector<oscilline::FilledTriangle> fills;
    CHECK(oscilline::paint_disc_figure(lines, pose, 200.f, 240.f, &fills));
    REQUIRE(lines.size() == 1);
    REQUIRE(fills.size() == 2);

    const float mid_x = (lines[0].x0 + lines[0].x1) * 0.5f;
    const float mid_y = (lines[0].y0 + lines[0].y1) * 0.5f;
    const oscilline::FilledTriangle* black = nullptr;
    const oscilline::FilledTriangle* blue = nullptr;
    for (const oscilline::FilledTriangle& fill : fills) {
        if (fill.color.r < 0.05f && fill.color.b < 0.05f) {
            black = &fill;
        } else if (fill.color.b > 0.95f) {
            blue = &fill;
        }
    }
    REQUIRE(black != nullptr);
    REQUIRE(blue != nullptr);
    CHECK(black->depth < lines[0].depth);
    CHECK(blue->depth > lines[0].depth);
    CHECK(point_in_triangle(
        mid_x, mid_y, black->x0, black->y0, black->x1, black->y1, black->x2, black->y2));

    oscilline::TriangleList triangles;
    oscilline::StrokeStyle course;
    course.width = oscilline::kCourseStrokeWidth;
    course.feather = oscilline::kCourseStrokeFeather;
    oscilline::append_strokes(triangles, lines, course, fills);

    int black_count = 0;
    int blue_count = 0;
    int white_count = 0;
    int first_black = -1;
    int first_blue = -1;
    int first_white = -1;
    for (std::size_t i = 0; i < triangles.vertices.size(); ++i) {
        const oscilline::Vertex& vertex = triangles.vertices[i];
        if (near_black(vertex)) {
            ++black_count;
            CHECK(vertex.a == doctest::Approx(1.f));
            if (first_black < 0) {
                first_black = static_cast<int>(i);
            }
        } else if (near_blue(vertex)) {
            ++blue_count;
            if (first_blue < 0) {
                first_blue = static_cast<int>(i);
            }
        } else if (near_white(vertex)) {
            ++white_count;
            if (first_white < 0) {
                first_white = static_cast<int>(i);
            }
        }
    }
    CHECK(black_count == 3);
    CHECK(blue_count == 3);
    CHECK(white_count > 3);
    CHECK(first_blue >= 0);
    CHECK(first_white > first_blue);
    CHECK(first_black > first_white);

    oscilline::FilledTriangle flat;
    flat.x0 = flat.x1 = flat.x2 = 12.f;
    flat.y0 = flat.y1 = flat.y2 = 12.f;
    oscilline::TriangleList degenerate;
    oscilline::append_strokes(
        degenerate, {}, {}, std::span<const oscilline::FilledTriangle>(&flat, 1));
    CHECK(degenerate.vertices.size() == 3);
    const oscilline::TriangleList dropped =
        oscilline::drop_degenerate_triangles(degenerate, 640.f, 480.f);
    CHECK(dropped.vertices.empty());
}

TEST_CASE("a quad is two filled triangles and the wireframe keeps its edges") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, 0}, {0, -10, 0}, {0, -10, 10}, {0, 0, 10}};
    object.primitives.push_back(polygon_prim({0, 1, 2, 3}, 0x00010100u));
    model.objects.push_back(object);
    oscilline::WireframeOptions options;
    options.packet_color = true;
    const oscilline::FigureMesh mesh = oscilline::tmd_figure_mesh(model, {}, options);
    CHECK(mesh.lines.empty());
    REQUIRE(mesh.triangles.size() == 2);
    CHECK(oscilline::tmd_wireframe(model).size() == 4);
    CHECK(mesh.triangles[0].color.r == doctest::Approx(0.f));
    CHECK(mesh.triangles[0].color.g == doctest::Approx(1.f / 255.f));
    CHECK(mesh.triangles[0].color.b == doctest::Approx(1.f / 255.f));
}

TEST_CASE("disc projection matches the road at the origin and ignores roll") {
    oscilline::DiscView view;
    view.eye_z = -100.f;
    view.projection_h = 100.f;
    oscilline::DiscProjected origin;
    REQUIRE(oscilline::project_disc_world(view, {0.f, 0.f, 0.f}, 640.f, 480.f, origin));
    CHECK(origin.x == doctest::Approx(320.f));
    CHECK(origin.y == doctest::Approx(124.f * 480.f / 286.f));
    CHECK(origin.depth == doctest::Approx(100.f));

    oscilline::DiscProjected right;
    REQUIRE(oscilline::project_disc_world(view, {10.f, 0.f, 0.f}, 640.f, 480.f, right));
    CHECK(right.x > origin.x + 1.f);
    CHECK(right.y == doctest::Approx(origin.y));

    oscilline::DiscProjected down;
    REQUIRE(oscilline::project_disc_world(view, {0.f, 10.f, 0.f}, 640.f, 480.f, down));
    CHECK(down.y > origin.y + 1.f);
    CHECK(down.x == doctest::Approx(origin.x));

    oscilline::DiscProjected behind;
    CHECK_FALSE(oscilline::project_disc_world(view, {0.f, 0.f, -200.f}, 640.f, 480.f, behind));
    view.projection_h = 0.f;
    CHECK_FALSE(oscilline::project_disc_world(view, {0.f, 0.f, 0.f}, 640.f, 480.f, origin));

    oscilline::DiscView up;
    up.eye_y = 5.f;
    up.projection_h = 80.f;
    oscilline::DiscProjected up_origin;
    oscilline::DiscProjected up_right;
    REQUIRE(oscilline::project_disc_world(up, {0.f, 0.f, 0.f}, 640.f, 480.f, up_origin));
    REQUIRE(oscilline::project_disc_world(up, {8.f, 0.f, 0.f}, 640.f, 480.f, up_right));
    CHECK(up_right.x > up_origin.x);

    oscilline::AncSample sample;
    sample.eye_x = 400.f;
    sample.eye_z = -30.f;
    sample.fov = 360.f;
    oscilline::DiscView matched;
    matched.eye_x = sample.eye_x;
    matched.eye_z = sample.eye_z;
    matched.projection_h = sample.fov;
    oscilline::DiscProjected projected;
    REQUIRE(oscilline::project_disc_world(matched, {0.f, 0.f, 0.f}, 640.f, 480.f, projected));
    const oscilline::DiscCameraFrame framed = oscilline::frame_disc_camera(sample, 640.f, 480.f);
    REQUIRE(framed.ok);
    CHECK(projected.x == doctest::Approx(framed.figure_x));
    CHECK(projected.y == doctest::Approx(framed.figure_y));
}

TEST_CASE("an S01-like side camera matches the side view within one pixel") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, 0}, {0, -100, 0}};
    object.primitives.push_back(line_prim(0, 1, 0x00FFFFFFu));
    model.objects.push_back(object);
    const oscilline::AnmFile clip = still_clip();
    constexpr float kScale = 0.88f;
    constexpr float kDistance = 1000.f;
    const float stretch_y =
        static_cast<float>(oscilline::kLogicalHeight) / oscilline::kDiscBufferHeight;
    const float projection_h = kScale * kDistance / (oscilline::kFigureWorldPerModel * stretch_y);
    const float fov = oscilline::kAncProjectionScale / projection_h;

    oscilline::DiscView view;
    view.eye_x = kDistance;
    view.projection_h = projection_h;
    oscilline::DiscProjected origin;
    REQUIRE(oscilline::project_disc_world(view, {0.f, 0.f, 0.f}, 640.f, 480.f, origin));

    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(kScale, 0.f, 0.f);

    std::vector<oscilline::Segment> side;
    CHECK(oscilline::paint_disc_figure(side, pose, origin.x, origin.y));
    REQUIRE(side.size() == 1);

    const oscilline::AncSample camera = side_sample(kDistance, fov, 0.f);
    std::vector<oscilline::Segment> projected;
    bool disc = false;
    CHECK(
        oscilline::paint_disc_figure(projected, pose, origin.x, origin.y, nullptr, &camera, &disc));
    CHECK(disc);
    REQUIRE(projected.size() == 1);

    const auto foot = [](const oscilline::Segment& segment) {
        return segment.y0 > segment.y1 ? segment.y0 : segment.y1;
    };
    const auto head = [](const oscilline::Segment& segment) {
        return segment.y0 < segment.y1 ? segment.y0 : segment.y1;
    };
    CHECK(projected[0].x0 == doctest::Approx(side[0].x0).epsilon(0.01));
    CHECK(projected[0].x1 == doctest::Approx(side[0].x1).epsilon(0.01));
    CHECK(std::fabs(foot(projected[0]) - foot(side[0])) < 1.f);
    CHECK(std::fabs(head(projected[0]) - head(side[0])) < 1.f);
    const float side_height = foot(side[0]) - head(side[0]);
    const float disc_height = foot(projected[0]) - head(projected[0]);
    CHECK(side_height == doctest::Approx(oscilline::kFigureHeightPx));
    CHECK(std::fabs(disc_height - side_height) / side_height < 0.05f);
}

TEST_CASE("a front camera is narrow and upright and roll does not tip the figure") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {
        {0, 0, 0},
        {0, -100, 0},
        {-4, -40, 0},
        {4, -40, 0},
        {0, -40, -25},
        {0, -40, 25},
    };
    object.primitives.push_back(line_prim(0, 1, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(2, 3, 0x00FFFFFFu));
    object.primitives.push_back(line_prim(4, 5, 0x00FFFFFFu));
    model.objects.push_back(object);
    const oscilline::AnmFile clip = still_clip();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(1.f, 0.f, 0.f);

    constexpr float kFov = 500.f;
    oscilline::AncSample front;
    front.eye_z = -1000.f;
    front.fov = kFov;
    oscilline::AncSample side = side_sample(1000.f, kFov, 0.f);

    std::vector<oscilline::Segment> front_lines;
    std::vector<oscilline::Segment> side_lines;
    bool front_disc = false;
    bool side_disc = false;
    CHECK(oscilline::paint_disc_figure(front_lines, pose, 0.f, 0.f, nullptr, &front, &front_disc));
    CHECK(oscilline::paint_disc_figure(side_lines, pose, 0.f, 0.f, nullptr, &side, &side_disc));
    CHECK(front_disc);
    CHECK(side_disc);
    REQUIRE(front_lines.size() == 3);
    REQUIRE(side_lines.size() == 3);

    float front_min_x = 0.f;
    float front_max_x = 0.f;
    float front_min_y = 0.f;
    float front_max_y = 0.f;
    float side_min_x = 0.f;
    float side_max_x = 0.f;
    float side_min_y = 0.f;
    float side_max_y = 0.f;
    segment_span(front_lines, front_min_x, front_max_x, front_min_y, front_max_y);
    segment_span(side_lines, side_min_x, side_max_x, side_min_y, side_max_y);
    const float front_width = front_max_x - front_min_x;
    const float side_width = side_max_x - side_min_x;
    CHECK(front_width > 1.f);
    CHECK(front_width < side_width * 0.4f);

    const oscilline::Segment* spine = &front_lines[0];
    for (const oscilline::Segment& line : front_lines) {
        if (std::fabs(line.y1 - line.y0) > std::fabs(spine->y1 - spine->y0)) {
            spine = &line;
        }
    }
    CHECK(std::fabs(spine->x0 - spine->x1) < 1.f);
    CHECK(std::min(spine->y0, spine->y1) < std::max(spine->y0, spine->y1) - 10.f);

    const oscilline::AncSample rolled = side_sample(1000.f, kFov, 1024.f);
    std::vector<oscilline::Segment> rolled_lines;
    CHECK(oscilline::paint_disc_figure(rolled_lines, pose, 0.f, 0.f, nullptr, &rolled));
    REQUIRE(rolled_lines.size() == side_lines.size());
    for (std::size_t i = 0; i < side_lines.size(); ++i) {
        CHECK(rolled_lines[i].x0 == doctest::Approx(side_lines[i].x0));
        CHECK(rolled_lines[i].y0 == doctest::Approx(side_lines[i].y0));
        CHECK(rolled_lines[i].x1 == doctest::Approx(side_lines[i].x1));
        CHECK(rolled_lines[i].y1 == doctest::Approx(side_lines[i].y1));
    }

    const oscilline::AncSample pitched[] = {
        side_sample(1000.f, 400.f, 800.f),
        side_sample(1000.f, 400.f, -400.f),
    };
    oscilline::AncSample steep;
    steep.eye_x = 220.f;
    steep.eye_y = -640.f;
    steep.eye_z = 90.f;
    steep.fov = 480.f;
    steep.roll = 1500.f;
    oscilline::AncSample ahead;
    ahead.eye_z = -800.f;
    ahead.fov = 420.f;
    ahead.roll = 2000.f;
    const oscilline::AncSample extras[] = {steep, ahead};
    for (const oscilline::AncSample& camera : pitched) {
        std::vector<oscilline::Segment> lines;
        CHECK(oscilline::paint_disc_figure(lines, pose, 0.f, 0.f, nullptr, &camera));
        REQUIRE_FALSE(lines.empty());
        const oscilline::Segment* tall = &lines[0];
        for (const oscilline::Segment& line : lines) {
            if (std::fabs(line.y1 - line.y0) > std::fabs(tall->y1 - tall->y0)) {
                tall = &line;
            }
        }
        CHECK(std::fabs(tall->x0 - tall->x1) < 1.f);
        CHECK(std::min(tall->y0, tall->y1) < std::max(tall->y0, tall->y1) - 5.f);
    }
    for (const oscilline::AncSample& camera : extras) {
        std::vector<oscilline::Segment> lines;
        CHECK(oscilline::paint_disc_figure(lines, pose, 0.f, 0.f, nullptr, &camera));
        REQUIRE_FALSE(lines.empty());
        const oscilline::Segment* tall = &lines[0];
        for (const oscilline::Segment& line : lines) {
            if (std::fabs(line.y1 - line.y0) > std::fabs(tall->y1 - tall->y0)) {
                tall = &line;
            }
        }
        CHECK(std::fabs(tall->x0 - tall->x1) < 1.f);
        CHECK(std::min(tall->y0, tall->y1) < std::max(tall->y0, tall->y1) - 5.f);
    }
}

TEST_CASE("the projected figure stays vertical and roll does not turn the shared ribbon") {
    oscilline::TmdModel model;
    oscilline::TmdObject object;
    object.vertices = {{0, 0, 0}, {0, -80, 0}};
    object.primitives.push_back(line_prim(0, 1, 0x000000FFu));
    model.objects.push_back(object);
    const oscilline::AnmFile clip = still_clip();
    oscilline::DiscFigurePose pose;
    pose.model = &model;
    pose.clip = &clip;
    pose.loop = false;
    pose.placement = placement_of(1.f, 0.f, 0.f);

    oscilline::AncFile play;
    oscilline::AncKeyframe key;
    key.eye_x = 1000;
    key.fov = 500;
    key.roll = 1024;
    play.keys.push_back(key);

    oscilline::CourseTimeline course;
    course.duration_ms = 60000;
    oscilline::PlayState state;
    oscilline::CourseView view;
    view.figure = &pose;
    view.cameras.play = &play;
    view.disc_camera = true;
    const oscilline::CourseFrame frame = oscilline::draw_course(course, state, 0, view);

    float red_min_x = 0.f;
    float red_max_x = 0.f;
    float red_min_y = 0.f;
    float red_max_y = 0.f;
    bool any_red = false;
    float white_min_y = 1e9f;
    float white_max_y = -1e9f;
    for (const oscilline::Vertex& vertex : frame.triangles.vertices) {
        if (near_red(vertex)) {
            if (!any_red) {
                red_min_x = red_max_x = vertex.x;
                red_min_y = red_max_y = vertex.y;
                any_red = true;
            } else {
                red_min_x = std::min(red_min_x, vertex.x);
                red_max_x = std::max(red_max_x, vertex.x);
                red_min_y = std::min(red_min_y, vertex.y);
                red_max_y = std::max(red_max_y, vertex.y);
            }
        } else if (near_white(vertex)) {
            white_min_y = std::min(white_min_y, vertex.y);
            white_max_y = std::max(white_max_y, vertex.y);
        }
    }
    REQUIRE(any_red);
    // One projection for the ribbon and the figure: the side camera keeps the
    // ribbon level, and her foot sits on it.
    CHECK((white_max_y - white_min_y) < 4.f);
    CHECK(std::fabs(red_max_y - (white_min_y + white_max_y) * 0.5f) < 2.f);
    CHECK((red_max_y - red_min_y) > (red_max_x - red_min_x) * 2.f);

    int first_red = -1;
    int last_white = -1;
    for (std::size_t i = 0; i < frame.triangles.vertices.size(); ++i) {
        const oscilline::Vertex& vertex = frame.triangles.vertices[i];
        if (near_red(vertex) && first_red < 0) {
            first_red = static_cast<int>(i);
        }
        if (near_white(vertex)) {
            last_white = static_cast<int>(i);
        }
    }
    CHECK(first_red > 0);
    CHECK(last_white >= 0);
    CHECK(first_red > last_white);
}

TEST_CASE("the progress arc color follows the fill fraction") {
    const auto extent = [](const std::vector<oscilline::Segment>& segments) {
        float min_x = segments.front().x0;
        float max_x = segments.front().x0;
        for (const oscilline::Segment& segment : segments) {
            min_x = std::min(min_x, std::min(segment.x0, segment.x1));
            max_x = std::max(max_x, std::max(segment.x0, segment.x1));
        }
        return std::pair{min_x, max_x};
    };
    const auto leftmost_yellow = [](const std::vector<oscilline::Segment>& segments) {
        float x = 1.e9f;
        for (const oscilline::Segment& segment : segments) {
            const bool yellow =
                segment.color.r > 0.9f && segment.color.g > 0.7f && segment.color.b < 0.4f;
            if (yellow) {
                x = std::min(x, std::min(segment.x0, segment.x1));
            }
        }
        return x;
    };

    const std::vector<oscilline::Segment> empty = oscilline::progress_arc_segments(0.f);
    const std::vector<oscilline::Segment> half = oscilline::progress_arc_segments(0.5f);
    const std::vector<oscilline::Segment> nudged =
        oscilline::progress_arc_segments(0.5f + 1.f / 64.f);
    const std::vector<oscilline::Segment> full = oscilline::progress_arc_segments(1.f);
    REQUIRE_FALSE(empty.empty());
    CHECK(empty.front().color.r == doctest::Approx(0.f));
    CHECK(empty.front().color.g == doctest::Approx(104.f / 255.f));
    CHECK(empty.front().color.b == doctest::Approx(88.f / 255.f));
    const auto [empty_left, empty_right] = extent(empty);
    const auto [full_left, full_right] = extent(full);
    CHECK(empty_left == doctest::Approx(50.f));
    CHECK(empty_right == doctest::Approx(590.f));
    CHECK(full_left == doctest::Approx(50.f));
    CHECK(full_right == doctest::Approx(590.f));
    CHECK(leftmost_yellow(empty) > 1.e8f);
    CHECK(leftmost_yellow(nudged) < leftmost_yellow(half) - 1.f);
    CHECK(leftmost_yellow(full) == doctest::Approx(104.f));
}

TEST_CASE("an obstacle remains drawn until its body leaves the left edge") {
    oscilline::PlayState play;
    play.form = oscilline::Form::Super;
    oscilline::CourseTimeline empty;
    empty.duration_ms = 60000;
    const oscilline::CourseFrame bare = oscilline::draw_course(empty, play, 5559);

    oscilline::CourseTimeline course = empty;
    oscilline::CourseEvent event;
    event.obstacle = 3;
    event.hit_ms = 5000;
    event.approach_ms = 2000;
    event.scroll_approach_ms = 2000;
    course.events.push_back(event);
    const oscilline::CourseFrame visible = oscilline::draw_course(course, play, 5559);
    CHECK(visible.triangles.vertices.size() > bare.triangles.vertices.size());
}

TEST_CASE("an obstacle past the audio end is not drawn") {
    oscilline::PlayState play;
    oscilline::CourseTimeline empty;
    empty.duration_ms = 60000;
    empty.audio_end_ms = 3000;
    const oscilline::CourseFrame bare = oscilline::draw_course(empty, play, 4000);

    oscilline::CourseTimeline late = empty;
    oscilline::CourseEvent event;
    event.obstacle = 0;
    event.hit_ms = 5000;
    event.approach_ms = 2000;
    event.scroll_approach_ms = 2000;
    late.events.push_back(event);
    const oscilline::CourseFrame hidden = oscilline::draw_course(late, play, 4000);
    CHECK(hidden.triangles.vertices.size() == bare.triangles.vertices.size());

    late.audio_end_ms = 0;
    const oscilline::CourseFrame shown = oscilline::draw_course(late, play, 4000);
    CHECK(shown.triangles.vertices.size() > bare.triangles.vertices.size());
}

TEST_CASE("the score can show as a number and the control line is opt-in") {
    using namespace oscilline;
    CourseTimeline course;
    course.duration_ms = 60000;
    PlayState play;
    play.score = 730;
    const auto draw = [&](const CourseHud& hud) {
        CourseView view;
        view.hud = hud;
        return draw_course(course, play, 1000, view);
    };
    const auto has_text = [](const CourseFrame& frame, std::string_view value) {
        return std::any_of(frame.text.begin(), frame.text.end(), [&](const TextGlyph& glyph) {
            return glyph.text == value;
        });
    };
    const CourseFrame coupons = draw({});
    CHECK_FALSE(has_text(coupons, "730"));
    for (const TextGlyph& glyph : coupons.text) {
        CHECK(glyph.text.find("BLOCK") == std::string::npos);
    }

    CourseHud hud;
    hud.score_number = true;
    hud.control_hint = "Q BLOCK   E LOOP   X WAVE   S PIT";
    const CourseFrame number = draw(hud);
    CHECK(has_text(number, "730"));
    CHECK(has_text(number, hud.control_hint));
    // The coupon strokes are gone.
    CHECK(number.triangles.vertices.size() < coupons.triangles.vertices.size());
}

TEST_CASE("impact jitter is stronger on the figure and obstacle") {
    using namespace oscilline;
    Segment base;
    base.x0 = 120.f;
    base.y0 = 80.f;
    base.x1 = 180.f;
    base.y1 = 160.f;
    std::vector<Segment> regular{base};
    std::vector<Segment> impact{base};
    constexpr float amplitude = 15.f;
    constexpr std::int64_t time = 460;
    jitter_figure_vertices(regular, time, amplitude, 0xF16u);
    jitter_figure_vertices(impact, time, amplitude, 0xF16u, 1.f);
    const float regular_dx = regular.front().x0 - base.x0;
    const float impact_dx = impact.front().x0 - base.x0;
    const float regular_dy = regular.front().y0 - base.y0;
    const float impact_dy = impact.front().y0 - base.y0;
    CHECK(std::fabs(impact_dx) >= std::fabs(regular_dx));
    CHECK(std::fabs(impact_dy) >= std::fabs(regular_dy));
    CHECK(std::fabs(impact_dx) + std::fabs(impact_dy) >
          std::fabs(regular_dx) + std::fabs(regular_dy));

    std::vector<Segment> obstacle_still;
    std::vector<Segment> obstacle_stage;
    std::vector<Segment> obstacle_impact;
    constexpr std::uint32_t salt = 0xA51u;
    append_jittered_obstacle(obstacle_still, 0, 220.f, 240.f, 80.f, 1.f, time, 0.f, salt);
    append_jittered_obstacle(obstacle_stage, 0, 220.f, 240.f, 80.f, 1.f, time, 5.f, salt);
    append_jittered_obstacle(obstacle_impact, 0, 220.f, 240.f, 80.f, 1.f, time, 14.f, salt);
    REQUIRE(obstacle_still.size() == obstacle_stage.size());
    REQUIRE(obstacle_stage.size() == obstacle_impact.size());
    float stage_delta = 0.f;
    float impact_delta = 0.f;
    for (std::size_t i = 0; i < obstacle_still.size(); ++i) {
        stage_delta += std::fabs(obstacle_stage[i].x0 - obstacle_still[i].x0) +
                       std::fabs(obstacle_stage[i].y0 - obstacle_still[i].y0) +
                       std::fabs(obstacle_stage[i].x1 - obstacle_still[i].x1) +
                       std::fabs(obstacle_stage[i].y1 - obstacle_still[i].y1);
        impact_delta += std::fabs(obstacle_impact[i].x0 - obstacle_still[i].x0) +
                        std::fabs(obstacle_impact[i].y0 - obstacle_still[i].y0) +
                        std::fabs(obstacle_impact[i].x1 - obstacle_still[i].x1) +
                        std::fabs(obstacle_impact[i].y1 - obstacle_still[i].y1);
    }
    CHECK(impact_delta > stage_delta);
}

TEST_CASE("the progress indicator stays at base stage jitter during a hit") {
    using namespace oscilline;
    CourseTimeline course;
    course.duration_ms = 60000;
    PlayState hit;
    hit.hits_since_form = 1;
    hit.last_hit_ms = 0;
    hit.last = Judgment::Miss;
    hit.judged_ms = 0;

    const CourseFrame frame = draw_course(course, hit, 0);
    const auto green_vertices = [](std::span<const Vertex> vertices) {
        std::vector<std::array<float, 2>> points;
        for (const Vertex& vertex : vertices) {
            if (vertex.y > 400.f && vertex.r < 0.05f && vertex.g > 0.35f &&
                vertex.b > 0.30f && vertex.b < 0.40f) {
                points.push_back({vertex.x, vertex.y});
            }
        }
        return points;
    };
    constexpr float kBaseStageAmplitudePs = 0.5f;
    constexpr std::uint32_t kProgressSalt = 0x6D6574u;
    std::vector<Segment> expected_segments = progress_arc_segments(0.f);
    jitter_segments(expected_segments, 0, kBaseStageAmplitudePs, kProgressSalt);
    TriangleList expected_triangles;
    append_strokes(expected_triangles, expected_segments);
    const auto actual_points = green_vertices(frame.triangles.vertices);
    const auto expected_points = green_vertices(expected_triangles.vertices);
    REQUIRE_FALSE(actual_points.empty());
    REQUIRE(actual_points.size() == expected_points.size());
    for (std::size_t i = 0; i < actual_points.size(); ++i) {
        CHECK(actual_points[i][0] == doctest::Approx(expected_points[i][0]));
        CHECK(actual_points[i][1] == doctest::Approx(expected_points[i][1]));
    }
}

TEST_CASE("taking damage does not add indicator strokes around the figure") {
    using namespace oscilline;
    CourseTimeline course;
    course.duration_ms = 60000;
    PlayState unharmed;
    PlayState hit;
    hit.damage = 1;

    const CourseFrame plain = draw_course(course, unharmed, 1000);
    const CourseFrame damaged = draw_course(course, hit, 1000);
    CHECK(damaged.triangles.vertices.size() == plain.triangles.vertices.size());
}

TEST_CASE("the gameplay HUD widens the progress arc and animates the round caption") {
    using namespace oscilline;
    CourseTimeline course;
    course.duration_ms = 60000;
    const PlayState play;
    CourseView view;
    view.round_number = 2;
    const auto frame_at = [&](std::int64_t elapsed) {
        return draw_course(course, play, elapsed - kCourseStartDelayMs, view);
    };
    const auto round_label = [](const CourseFrame& frame) -> const TextGlyph* {
        for (const TextGlyph& glyph : frame.text) {
            if (glyph.text == "Round 2") {
                return &glyph;
            }
        }
        return nullptr;
    };

    const auto arc = progress_arc_segments(0.f);
    REQUIRE_FALSE(arc.empty());
    float min_x = arc.front().x0;
    float max_x = arc.front().x0;
    for (const Segment& line : arc) {
        min_x = std::min({min_x, line.x0, line.x1});
        max_x = std::max({max_x, line.x0, line.x1});
    }
    CHECK(min_x == doctest::Approx(50.f));
    CHECK(max_x == doctest::Approx(590.f));

    const CourseFrame start = frame_at(0);
    const TextGlyph* initial = round_label(start);
    REQUIRE(initial != nullptr);
    CHECK(initial->scale == doctest::Approx(2.5f));
    CHECK(initial->horizontal_scale == doctest::Approx(0.70f));
    CHECK(std::fabs(initial->x + initial->scale * initial->horizontal_scale * 8.f * 7.f -
                    static_cast<float>(logical_width()) + 16.f + 64.f) < 0.01f);
    CHECK(std::none_of(start.text.begin(), start.text.end(), [](const TextGlyph& glyph) {
        return glyph.text == "COURSE 1";
    }));

    const TextGlyph* waiting = round_label(frame_at(2500));
    REQUIRE(waiting != nullptr);
    CHECK(waiting->x == doctest::Approx(initial->x));
    for (const std::int64_t blink_off : {3225, 3525, 3825, 4125}) {
        CHECK(round_label(frame_at(blink_off)) == nullptr);
    }
    const TextGlyph* pause = round_label(frame_at(4350));
    REQUIRE(pause != nullptr);
    CHECK(pause->x == doctest::Approx(initial->x));
    const TextGlyph* flying = round_label(frame_at(4450));
    REQUIRE(flying != nullptr);
    CHECK(flying->x == doctest::Approx(initial->x));
    const TextGlyph* halfway = round_label(frame_at(4900));
    REQUIRE(halfway != nullptr);
    CHECK(halfway->x > initial->x + logical_width() * 0.4f);
    CHECK(round_label(frame_at(5350)) == nullptr);
}
