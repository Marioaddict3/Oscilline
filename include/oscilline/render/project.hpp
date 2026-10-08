// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// TMD posing, wireframe edges, filled figure meshes, and projection.

#pragma once

#include "oscilline/anm.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"
#include "oscilline/tmd.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace oscilline {

// Wireframe camera. Perspective looks from +Z toward the origin. Orthographic
// ignores depth except for painter's order and is what the unit tests use.
struct Camera {
    enum class Mode {
        Perspective,
        Orthographic,
    };

    Mode mode = Mode::Perspective;
    float eye_z = 512.f;
    float fov_y_radians = 0.9f;
    float ortho_pixels_per_unit = 1.f;
    float width = static_cast<float>(logical_width());
    float height = static_cast<float>(kLogicalHeight);
    float near_plane = 1.f;
    // Point the camera looks at. Model coordinates are taken relative to it.
    float target_x = 0;
    float target_y = 0;
    float target_z = 0;
};

struct ModelVertex {
    float x = 0;
    float y = 0;
    float z = 0;
};

struct ModelSegment {
    ModelVertex a;
    ModelVertex b;
    // White unless a caller asks the wireframe to copy the packet color.
    Rgb color{};
    // The TMD object this came from. Set by tmd_figure_mesh.
    std::uint32_t object = 0;
};

struct ModelTriangle {
    ModelVertex a;
    ModelVertex b;
    ModelVertex c;
    Rgb color{};
    // The TMD object this came from. Set by tmd_figure_mesh.
    std::uint32_t object = 0;
    // An eye fill: the object's only fill is pure black, inside an outline of
    // at least kEyeOutlineLines lines. It is drawn behind that outline.
    bool under_own_lines = false;
};

inline constexpr int kEyeOutlineLines = 8;

// Lines stay strokes. Polygon primitives are filled triangles (a quad is two).
// The course figure draws these together, far to near. tmd_wireframe still
// turns polygons into edges for the viewer and the HUD.
struct FigureMesh {
    std::vector<ModelSegment> lines;
    std::vector<ModelTriangle> triangles;
};

struct WireframeOptions {
    // Packet colors are 0x00BBGGRR. Off leaves every segment white.
    bool packet_color = false;
    // Skip the head occluders: the 0x010000 triangle inside each head outline.
    // It does not cover the outline, so it shows as a stray black triangle.
    bool skip_black_occluders = false;
};

struct ScreenRect {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

// One TMD object's pose for a single animation frame. An object with no key
// in that frame is not drawn (open-ribbon ANM notes).
struct Pose {
    bool visible = false;
    float rotation_x = 0;
    float rotation_y = 0;
    float rotation_z = 0;
    float scale_x = 1.f;
    float scale_y = 1.f;
    float scale_z = 1.f;
    float position_x = 0;
    float position_y = 0;
    float position_z = 0;
};

// TMD and ANM data use the PlayStation convention: +X right, +Y down, +Z away
// from the viewer. The renderer is +Y up with the camera on +Z. `Side` looks
// at the model from its +X side, so the model's +Z becomes screen right (the
// in-game profile). `Front` looks along the model's +Z. Both are rotations,
// never mirrors.
enum class ModelView {
    Side,
    Front,
};

[[nodiscard]] ModelVertex to_view_space(ModelVertex point, ModelView view);
[[nodiscard]] std::vector<ModelSegment> to_view_space(std::span<const ModelSegment> model,
                                                      ModelView view);

struct ModelBounds {
    bool empty = true;
    ModelVertex min;
    ModelVertex max;
};

void extend_bounds(ModelBounds& bounds, std::span<const ModelSegment> model);

// Perspective camera aimed at the center of `bounds`, far enough back that
// the whole box fits the logical view with a margin. An empty box returns `base`.
[[nodiscard]] Camera framing_camera(const ModelBounds& bounds, Camera base = {});

// Projects model segments. A perspective segment with an endpoint on the near
// side of the camera is dropped rather than clipped.
[[nodiscard]] std::vector<Segment> project_segments(std::span<const ModelSegment> model,
                                                    const Camera& camera);

// `poses` empty: every object is drawn in bind pose.
// Otherwise an object is drawn only when its pose is visible. Poses past the
// object count are ignored; objects past `poses` stay hidden.
//
// Assumptions, both local to this translation:
// - TmdObject::scale is a bit shift applied to vertices before the pose.
//   Positive shifts left, negative shifts right, clamped to 16 bits.
// - ANM rotation is applied Rx, then Ry, then Rz. Radians are value * 2π / 4096
//   and scale is value / 4096, as in the ANM notes. Position is the raw int16.
[[nodiscard]] std::vector<ModelSegment> tmd_wireframe(const TmdModel& model,
                                                      std::span<const Pose> poses = {},
                                                      WireframeOptions options = {});

// Same pose rules as tmd_wireframe. Line primitives become segments. Polygon
// primitives become filled triangles in packet color, not edge loops.
[[nodiscard]] FigureMesh tmd_figure_mesh(const TmdModel& model,
                                         std::span<const Pose> poses = {},
                                         WireframeOptions options = {});

// Eye, look-at target, and projection distance H. Axes match the ANC keys:
// +X right, +Y down, +Z forward. Roll is not applied.
struct DiscView {
    float eye_x = 0;
    float eye_y = 0;
    float eye_z = 0;
    float target_x = 0;
    float target_y = 0;
    float target_z = 0;
    float projection_h = 0;
};

struct DiscProjected {
    float x = 0;
    float y = 0;
    float depth = 0;
};

// Same natural projection the disc camera uses for the road, without the roll
// channel. In the 512×286 frame, x = 256 + H * right / z and
// y = 124 + H * down / z, with right = normalize(cross((0, 1, 0), forward))
// and down = cross(forward, right). The frame then stretches onto
// `screen_width` × `screen_height` (640/512 and 480/286 on the logical view,
// the latter stated as 1.678). False when the look is degenerate, H is not
// positive, or the point is behind the camera. `depth` is camera-space z.
[[nodiscard]] bool project_disc_world(const DiscView& view,
                                      ModelVertex world,
                                      float screen_width,
                                      float screen_height,
                                      DiscProjected& out);

// Camera-space depth of `world` for `view`, the `depth` project_disc_world reports.
// False when the look is degenerate. Callers clip against it before projecting.
[[nodiscard]] bool disc_view_depth(const DiscView& view, ModelVertex world, float& depth);

// Uniform fit of model XY into `rect`. Model +Y is up. Colors are copied.
// A degenerate rectangle or an empty span returns no segments.
[[nodiscard]] std::vector<Segment> fit_model_xy(std::span<const ModelSegment> lines,
                                                ScreenRect rect);

// `fraction` blends from `frame` toward the next frame, wrapping at the end.
// Blending is skipped when either pose is hidden. `fraction` is clamped.
[[nodiscard]] std::vector<Pose> poses_for_frame(const AnmFile& animation,
                                                std::size_t object_count,
                                                int frame,
                                                float fraction,
                                                bool interpolate);

// The second ANM header field is unused in the format notes. Observed values
// 1, 10, 15, 20, 30, and 60 are a playback rate in frames per second.
// Anything else is 30.
[[nodiscard]] float anm_playback_hz(std::int16_t header_rate);

} // namespace oscilline
