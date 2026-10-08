// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Pose, side and front view, and the disc-world divide.

#include "oscilline/render/project.hpp"

#include "oscilline/course/camera.hpp"
#include "oscilline/render/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oscilline {
namespace {

constexpr float kTwoPi = std::numbers::pi_v<float> * 2.f;
constexpr float kAnmTurn = kTwoPi / 4096.f;
constexpr float kAnmScale = 1.f / 4096.f;
constexpr int kMaxShift = 16;
// The play models fill each head outline with one 0x010000 triangle that does
// not cover the outline. Eye fills use 0x000000.
constexpr std::uint32_t kHeadOccluderColor = 0x010000u;

float shifted(std::int16_t coordinate, std::int32_t scale) {
    int shift = static_cast<int>(scale);
    if (shift > kMaxShift) {
        shift = kMaxShift;
    }
    if (shift < -kMaxShift) {
        shift = -kMaxShift;
    }
    const float value = static_cast<float>(coordinate);
    if (shift >= 0) {
        return value * static_cast<float>(1u << static_cast<unsigned>(shift));
    }
    return value / static_cast<float>(1u << static_cast<unsigned>(-shift));
}

ModelVertex bind_vertex(const TmdVertex& vertex, std::int32_t scale) {
    ModelVertex out;
    out.x = shifted(vertex.x, scale);
    out.y = shifted(vertex.y, scale);
    out.z = shifted(vertex.z, scale);
    return out;
}

ModelVertex apply_pose(ModelVertex point, const Pose& pose) {
    point.x *= pose.scale_x;
    point.y *= pose.scale_y;
    point.z *= pose.scale_z;

    const float rx_c = std::cos(pose.rotation_x);
    const float rx_s = std::sin(pose.rotation_x);
    const float y_after_x = point.y * rx_c - point.z * rx_s;
    const float z_after_x = point.y * rx_s + point.z * rx_c;
    point.y = y_after_x;
    point.z = z_after_x;

    const float ry_c = std::cos(pose.rotation_y);
    const float ry_s = std::sin(pose.rotation_y);
    const float x_after_y = point.x * ry_c + point.z * ry_s;
    const float z_after_y = -point.x * ry_s + point.z * ry_c;
    point.x = x_after_y;
    point.z = z_after_y;

    const float rz_c = std::cos(pose.rotation_z);
    const float rz_s = std::sin(pose.rotation_z);
    const float x_after_z = point.x * rz_c - point.y * rz_s;
    const float y_after_z = point.x * rz_s + point.y * rz_c;
    point.x = x_after_z;
    point.y = y_after_z;

    point.x += pose.position_x;
    point.y += pose.position_y;
    point.z += pose.position_z;
    return point;
}

bool project_point(const Camera& camera, ModelVertex point, float& x, float& y, float& depth) {
    point.x -= camera.target_x;
    point.y -= camera.target_y;
    point.z -= camera.target_z;
    if (camera.mode == Camera::Mode::Orthographic) {
        x = camera.width * 0.5f + point.x * camera.ortho_pixels_per_unit;
        y = camera.height * 0.5f - point.y * camera.ortho_pixels_per_unit;
        depth = -point.z;
        return true;
    }
    const float z_cam = camera.eye_z - point.z;
    if (!(z_cam > camera.near_plane)) {
        return false;
    }
    const float focal = (camera.height * 0.5f) / std::tan(camera.fov_y_radians * 0.5f);
    x = camera.width * 0.5f + (point.x / z_cam) * focal;
    y = camera.height * 0.5f - (point.y / z_cam) * focal;
    depth = z_cam;
    return true;
}

Pose pose_from_key(const AnmKeyframe& key) {
    Pose pose;
    pose.visible = true;
    if (key.has_rotation) {
        pose.rotation_x = static_cast<float>(key.rotation_x) * kAnmTurn;
        pose.rotation_y = static_cast<float>(key.rotation_y) * kAnmTurn;
        pose.rotation_z = static_cast<float>(key.rotation_z) * kAnmTurn;
    }
    if (key.has_scale) {
        pose.scale_x = static_cast<float>(key.scale_x) * kAnmScale;
        pose.scale_y = static_cast<float>(key.scale_y) * kAnmScale;
        pose.scale_z = static_cast<float>(key.scale_z) * kAnmScale;
    }
    if (key.has_position) {
        pose.position_x = static_cast<float>(key.position_x);
        pose.position_y = static_cast<float>(key.position_y);
        pose.position_z = static_cast<float>(key.position_z);
    }
    return pose;
}

void apply_frame(std::vector<Pose>& poses, const AnmFrame& frame) {
    for (Pose& pose : poses) {
        pose = Pose{};
    }
    for (const AnmKeyframe& key : frame.keys) {
        if (key.object_index >= poses.size()) {
            continue;
        }
        poses[key.object_index] = pose_from_key(key);
    }
}

float wrap_angle(float radians) {
    float wrapped = std::fmod(radians, kTwoPi);
    if (wrapped > std::numbers::pi_v<float>) {
        wrapped -= kTwoPi;
    } else if (wrapped < -std::numbers::pi_v<float>) {
        wrapped += kTwoPi;
    }
    return wrapped;
}

float blend_angle(float from, float to, float fraction) {
    return from + wrap_angle(to - from) * fraction;
}

Pose blend_pose(const Pose& from, const Pose& to, float fraction) {
    if (!from.visible || !to.visible) {
        return from;
    }
    Pose pose = from;
    pose.rotation_x = blend_angle(from.rotation_x, to.rotation_x, fraction);
    pose.rotation_y = blend_angle(from.rotation_y, to.rotation_y, fraction);
    pose.rotation_z = blend_angle(from.rotation_z, to.rotation_z, fraction);
    pose.scale_x = from.scale_x + (to.scale_x - from.scale_x) * fraction;
    pose.scale_y = from.scale_y + (to.scale_y - from.scale_y) * fraction;
    pose.scale_z = from.scale_z + (to.scale_z - from.scale_z) * fraction;
    pose.position_x = from.position_x + (to.position_x - from.position_x) * fraction;
    pose.position_y = from.position_y + (to.position_y - from.position_y) * fraction;
    pose.position_z = from.position_z + (to.position_z - from.position_z) * fraction;
    return pose;
}

const TmdVertex* vertex_at(const TmdObject& object, std::uint16_t index) {
    if (index >= object.vertices.size()) {
        return nullptr;
    }
    return &object.vertices[index];
}

Rgb packet_rgb(std::uint32_t packed) {
    Rgb color;
    color.r = static_cast<float>(packed & 0xFFu) / 255.f;
    color.g = static_cast<float>((packed >> 8) & 0xFFu) / 255.f;
    color.b = static_cast<float>((packed >> 16) & 0xFFu) / 255.f;
    return color;
}

void add_edge(std::vector<ModelSegment>& out,
              const TmdObject& object,
              const Pose* pose,
              std::uint16_t index_a,
              std::uint16_t index_b,
              Rgb color) {
    const TmdVertex* a = vertex_at(object, index_a);
    const TmdVertex* b = vertex_at(object, index_b);
    if (a == nullptr || b == nullptr) {
        return;
    }
    ModelSegment segment;
    segment.a = bind_vertex(*a, object.scale);
    segment.b = bind_vertex(*b, object.scale);
    segment.color = color;
    if (pose != nullptr) {
        segment.a = apply_pose(segment.a, *pose);
        segment.b = apply_pose(segment.b, *pose);
    }
    out.push_back(segment);
}

bool posed_vertex(const TmdObject& object,
                  const Pose* pose,
                  std::uint16_t index,
                  ModelVertex& out) {
    const TmdVertex* vertex = vertex_at(object, index);
    if (vertex == nullptr) {
        return false;
    }
    out = bind_vertex(*vertex, object.scale);
    if (pose != nullptr) {
        out = apply_pose(out, *pose);
    }
    return true;
}

template <typename Item>
void tag_object(std::vector<Item>& items, std::size_t from, std::size_t object_index) {
    for (std::size_t i = from; i < items.size(); ++i) {
        items[i].object = static_cast<std::uint32_t>(object_index);
    }
}

void add_model_triangle(std::vector<ModelTriangle>& out,
                        const TmdObject& object,
                        const Pose* pose,
                        std::uint16_t index_a,
                        std::uint16_t index_b,
                        std::uint16_t index_c,
                        Rgb color) {
    ModelTriangle triangle;
    triangle.color = color;
    if (!posed_vertex(object, pose, index_a, triangle.a) ||
        !posed_vertex(object, pose, index_b, triangle.b) ||
        !posed_vertex(object, pose, index_c, triangle.c)) {
        return;
    }
    out.push_back(triangle);
}

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vec3 vec_sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float vec_dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float vec_length(Vec3 v) {
    return std::sqrt(vec_dot(v, v));
}

Vec3 vec_cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 vec_scale(Vec3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

} // namespace

ModelVertex to_view_space(ModelVertex point, ModelView view) {
    ModelVertex out;
    if (view == ModelView::Side) {
        out.x = point.z;
        out.y = -point.y;
        out.z = point.x;
    } else {
        out.x = point.x;
        out.y = -point.y;
        out.z = -point.z;
    }
    return out;
}

std::vector<ModelSegment> to_view_space(std::span<const ModelSegment> model, ModelView view) {
    std::vector<ModelSegment> out;
    out.reserve(model.size());
    for (const ModelSegment& segment : model) {
        ModelSegment mapped;
        mapped.a = to_view_space(segment.a, view);
        mapped.b = to_view_space(segment.b, view);
        mapped.color = segment.color;
        out.push_back(mapped);
    }
    return out;
}

void extend_bounds(ModelBounds& bounds, std::span<const ModelSegment> model) {
    const auto add = [&](const ModelVertex& point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
            return;
        }
        if (bounds.empty) {
            bounds.min = point;
            bounds.max = point;
            bounds.empty = false;
            return;
        }
        bounds.min.x = std::min(bounds.min.x, point.x);
        bounds.min.y = std::min(bounds.min.y, point.y);
        bounds.min.z = std::min(bounds.min.z, point.z);
        bounds.max.x = std::max(bounds.max.x, point.x);
        bounds.max.y = std::max(bounds.max.y, point.y);
        bounds.max.z = std::max(bounds.max.z, point.z);
    };
    for (const ModelSegment& segment : model) {
        add(segment.a);
        add(segment.b);
    }
}

Camera framing_camera(const ModelBounds& bounds, Camera base) {
    if (bounds.empty) {
        return base;
    }
    constexpr float kMargin = 1.15f;
    base.mode = Camera::Mode::Perspective;
    base.target_x = (bounds.min.x + bounds.max.x) * 0.5f;
    base.target_y = (bounds.min.y + bounds.max.y) * 0.5f;
    base.target_z = (bounds.min.z + bounds.max.z) * 0.5f;
    const float half_w = std::max((bounds.max.x - bounds.min.x) * 0.5f, 1.f);
    const float half_h = std::max((bounds.max.y - bounds.min.y) * 0.5f, 1.f);
    const float half_d = (bounds.max.z - bounds.min.z) * 0.5f;
    const float tan_y = std::tan(base.fov_y_radians * 0.5f);
    const float tan_x = tan_y * (base.width / base.height);
    const float distance = std::max(half_h / tan_y, half_w / tan_x) * kMargin;
    // The camera sits on +Z, in front of the nearest point of the box.
    base.eye_z = distance + half_d;
    return base;
}

std::vector<Segment> project_segments(std::span<const ModelSegment> model, const Camera& camera) {
    std::vector<Segment> segments;
    segments.reserve(model.size());
    for (const ModelSegment& source : model) {
        Segment segment;
        float depth_a = 0;
        float depth_b = 0;
        if (!project_point(camera, source.a, segment.x0, segment.y0, depth_a) ||
            !project_point(camera, source.b, segment.x1, segment.y1, depth_b)) {
            continue;
        }
        segment.depth = (depth_a + depth_b) * 0.5f;
        segment.color = source.color;
        segments.push_back(segment);
    }
    return segments;
}

std::vector<ModelSegment>
tmd_wireframe(const TmdModel& model, std::span<const Pose> poses, WireframeOptions options) {
    std::vector<ModelSegment> segments;
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const Pose* pose = nullptr;
        if (!poses.empty()) {
            if (object_index >= poses.size() || !poses[object_index].visible) {
                continue;
            }
            pose = &poses[object_index];
        }
        const TmdObject& object = model.objects[object_index];
        for (const TmdPrimitive& primitive : object.primitives) {
            const Rgb color = options.packet_color ? packet_rgb(primitive.color) : Rgb{};
            const auto& index = primitive.vertex_indices;
            if (primitive.kind == PrimitiveKind::Line && index.size() >= 2) {
                add_edge(segments, object, pose, index[0], index[1], color);
            } else if (primitive.kind == PrimitiveKind::Polygon && index.size() >= 3) {
                add_edge(segments, object, pose, index[0], index[1], color);
                add_edge(segments, object, pose, index[1], index[2], color);
                if (index.size() >= 4) {
                    add_edge(segments, object, pose, index[2], index[3], color);
                    add_edge(segments, object, pose, index[3], index[0], color);
                } else {
                    add_edge(segments, object, pose, index[2], index[0], color);
                }
            }
        }
    }
    return segments;
}

FigureMesh
tmd_figure_mesh(const TmdModel& model, std::span<const Pose> poses, WireframeOptions options) {
    FigureMesh mesh;
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const Pose* pose = nullptr;
        if (!poses.empty()) {
            if (object_index >= poses.size() || !poses[object_index].visible) {
                continue;
            }
            pose = &poses[object_index];
        }
        const TmdObject& object = model.objects[object_index];
        int outline_lines = 0;
        int black_fills = 0;
        int other_fills = 0;
        for (const TmdPrimitive& primitive : object.primitives) {
            if (primitive.kind == PrimitiveKind::Line) {
                ++outline_lines;
            } else if (primitive.kind == PrimitiveKind::Polygon) {
                ++((primitive.color & 0x00FFFFFFu) == 0u ? black_fills : other_fills);
            }
        }
        const bool eye = outline_lines >= kEyeOutlineLines && black_fills > 0 && other_fills == 0;
        const std::size_t triangles_start = mesh.triangles.size();
        for (const TmdPrimitive& primitive : object.primitives) {
            const std::size_t lines_before = mesh.lines.size();
            const Rgb color = options.packet_color ? packet_rgb(primitive.color) : Rgb{};
            const auto& index = primitive.vertex_indices;
            if (primitive.kind == PrimitiveKind::Line && index.size() >= 2) {
                add_edge(mesh.lines, object, pose, index[0], index[1], color);
                tag_object(mesh.lines, lines_before, object_index);
            } else if (primitive.kind == PrimitiveKind::Polygon && index.size() >= 3) {
                // The packet's top byte is the primitive code, so compare the color only.
                if (options.skip_black_occluders &&
                    (primitive.color & 0x00FFFFFFu) == kHeadOccluderColor) {
                    continue;
                }
                const std::size_t triangles_before = mesh.triangles.size();
                add_model_triangle(
                    mesh.triangles, object, pose, index[0], index[1], index[2], color);
                if (index.size() >= 4) {
                    add_model_triangle(
                        mesh.triangles, object, pose, index[0], index[2], index[3], color);
                }
                tag_object(mesh.triangles, triangles_before, object_index);
            }
        }
        for (std::size_t i = triangles_start; eye && i < mesh.triangles.size(); ++i) {
            mesh.triangles[i].under_own_lines = true;
        }
    }
    return mesh;
}

bool project_disc_world(const DiscView& view,
                        ModelVertex world,
                        float screen_width,
                        float screen_height,
                        DiscProjected& out) {
    if (!(view.projection_h > 0.f) || !std::isfinite(view.projection_h)) {
        return false;
    }
    if (!(screen_width > 0.f)) {
        screen_width = static_cast<float>(logical_width());
    }
    if (!(screen_height > 0.f)) {
        screen_height = static_cast<float>(kLogicalHeight);
    }
    const Vec3 eye{view.eye_x, view.eye_y, view.eye_z};
    const Vec3 target{view.target_x, view.target_y, view.target_z};
    const Vec3 forward_raw = vec_sub(target, eye);
    const float forward_len = vec_length(forward_raw);
    if (!(forward_len > 1.e-4f)) {
        return false;
    }
    const Vec3 forward = vec_scale(forward_raw, 1.f / forward_len);
    // right = normalize(cross((0, 1, 0), forward)). World +Y is down. No roll.
    Vec3 right_raw = vec_cross(Vec3{0.f, 1.f, 0.f}, forward);
    float right_len = vec_length(right_raw);
    if (!(right_len > 1.e-4f)) {
        right_raw = vec_cross(Vec3{0.f, 0.f, 1.f}, forward);
        right_len = vec_length(right_raw);
    }
    if (!(right_len > 1.e-4f)) {
        return false;
    }
    const Vec3 right = vec_scale(right_raw, 1.f / right_len);
    const Vec3 down = vec_cross(forward, right);
    const Vec3 point{world.x, world.y, world.z};
    const Vec3 delta = vec_sub(point, eye);
    const float z = vec_dot(delta, forward);
    if (!(z > 1.e-3f)) {
        return false;
    }
    const float ps_x = kDiscCenterX + view.projection_h * vec_dot(delta, right) / z;
    const float ps_y = kDiscCenterY + view.projection_h * vec_dot(delta, down) / z;
    out.x = ps_x * (screen_width / kDiscBufferWidth);
    out.y = ps_y * (screen_height / kDiscBufferHeight);
    out.depth = z;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.depth);
}

bool disc_view_depth(const DiscView& view, ModelVertex world, float& depth) {
    const Vec3 eye{view.eye_x, view.eye_y, view.eye_z};
    const Vec3 forward_raw = vec_sub(Vec3{view.target_x, view.target_y, view.target_z}, eye);
    const float forward_len = vec_length(forward_raw);
    if (!(forward_len > 1.e-4f)) {
        return false;
    }
    depth = vec_dot(vec_sub(Vec3{world.x, world.y, world.z}, eye), forward_raw) / forward_len;
    return std::isfinite(depth);
}

std::vector<Segment> fit_model_xy(std::span<const ModelSegment> lines, ScreenRect rect) {
    std::vector<Segment> out;
    const float rect_w = rect.right - rect.left;
    const float rect_h = rect.bottom - rect.top;
    if (lines.empty() || !(rect_w > 0.f) || !(rect_h > 0.f)) {
        return out;
    }
    float min_x = 0.f;
    float min_y = 0.f;
    float max_x = 0.f;
    float max_y = 0.f;
    bool any = false;
    const auto include = [&](const ModelVertex& point) {
        if (!any) {
            min_x = max_x = point.x;
            min_y = max_y = point.y;
            any = true;
            return;
        }
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    };
    for (const ModelSegment& line : lines) {
        include(line.a);
        include(line.b);
    }
    if (!any) {
        return out;
    }
    const float model_w = std::max(0.f, max_x - min_x);
    const float model_h = std::max(0.f, max_y - min_y);
    float scale = 1.f;
    if (model_w > 0.f && model_h > 0.f) {
        scale = std::min(rect_w / model_w, rect_h / model_h);
    } else if (model_w > 0.f) {
        scale = rect_w / model_w;
    } else if (model_h > 0.f) {
        scale = rect_h / model_h;
    }
    const float rect_cx = (rect.left + rect.right) * 0.5f;
    const float rect_cy = (rect.top + rect.bottom) * 0.5f;
    const float model_cx = (min_x + max_x) * 0.5f;
    const float model_cy = (min_y + max_y) * 0.5f;
    const auto map_point = [&](const ModelVertex& point, float& x, float& y) {
        x = rect_cx + (point.x - model_cx) * scale;
        y = rect_cy - (point.y - model_cy) * scale;
    };
    out.reserve(lines.size());
    for (const ModelSegment& line : lines) {
        Segment segment;
        map_point(line.a, segment.x0, segment.y0);
        map_point(line.b, segment.x1, segment.y1);
        segment.color = line.color;
        out.push_back(segment);
    }
    return out;
}

std::vector<Pose> poses_for_frame(const AnmFile& animation,
                                  std::size_t object_count,
                                  int frame,
                                  float fraction,
                                  bool interpolate) {
    std::vector<Pose> poses(object_count);
    if (animation.frames.empty() || object_count == 0) {
        return poses;
    }
    if (frame < 0) {
        frame = 0;
    }
    const int count = static_cast<int>(animation.frames.size());
    frame %= count;
    if (!std::isfinite(fraction)) {
        fraction = 0.f;
    }
    fraction = std::clamp(fraction, 0.f, 1.f);
    apply_frame(poses, animation.frames[static_cast<std::size_t>(frame)]);
    if (!interpolate || fraction == 0.f || count < 2) {
        return poses;
    }
    std::vector<Pose> next(object_count);
    const int next_frame = (frame + 1) % count;
    apply_frame(next, animation.frames[static_cast<std::size_t>(next_frame)]);
    for (std::size_t i = 0; i < poses.size(); ++i) {
        poses[i] = blend_pose(poses[i], next[i], fraction);
    }
    return poses;
}

float anm_playback_hz(std::int16_t header_rate) {
    switch (header_rate) {
    case 1:
    case 10:
    case 15:
    case 20:
    case 30:
    case 60:
        return static_cast<float>(header_rate);
    default:
        return 30.f;
    }
}

} // namespace oscilline
