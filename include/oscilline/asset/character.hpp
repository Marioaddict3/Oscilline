// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Play-form models, clip selection, and the disc-figure scale.

#pragma once

#include "oscilline/anc.hpp"
#include "oscilline/anm.hpp"
#include "oscilline/asset/map.hpp"
#include "oscilline/asset/registry.hpp"
#include "oscilline/course/play.hpp"
#include "oscilline/render/project.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/tmd.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oscilline {

// Play forms use the high-confidence character folders. Out has no model.
// Super uses the upgraded folder. Its clips follow the rabbit's stems.
[[nodiscard]] std::optional<Slot> form_model_slot(Form form);

struct ClipMatch {
    bool matched = false;
    // Trailing _F on an action stem. Idle variants stay false.
    bool whiff = false;
    ClipKind kind = ClipKind::Action;
    std::uint8_t actions = 0;
    Confidence confidence = Confidence::Low;
};

// Classifies one animation path. Unknown stems do not match.
[[nodiscard]] ClipMatch match_character_clip(std::string_view path);

// Indices into the form's animation list. -1 is absent. Entries below
// `minimum` are left unset. Every matched stem is high: singles, pair stems
// (JH, JL, JW, HL, HW, LW), trailing _F whiffs, idle variants, and miss clips.
// `whiff` holds an _F clip. A whiff with no _F stem uses `action`. SUPER is
// not a clip name.
struct CharacterLibrary {
    bool usable = false;
    int idle = -1;
    int action[16]{};
    int whiff[16]{};
    // An _F stem exists for this mask. A whiff plays it when the stem is
    // admitted. A mask with no _F stem falls back to the plain clip.
    bool whiff_stem[16]{};
    int miss[16]{};

    CharacterLibrary();
};

[[nodiscard]] CharacterLibrary character_library(const std::vector<std::string>& names,
                                                 Confidence minimum);

// Idle feet, in view space, and a scale that keeps the idle figure near the
// placeholder's height. Built once from the idle clip so later frames can
// leave the ribbon.
struct CharacterPlacement {
    bool valid = false;
    float scale = 1.f;
    float anchor_x = 0.f;
    float ground_y = 0.f;
};

inline constexpr float kFigureHeightPx = 88.f;
inline constexpr float kFigureMaxWidthPx = 140.f;
// World units per posed model unit. Through the play camera this is a 49-row
// rabbit in the 286-row frame. The original measures 49 rows (p10–p90 44–54).
// High confidence for that row count; mid-intro scale between keys ~105–135 is medium.
inline constexpr float kFigureWorldPerModel = 0.71f;

struct CharacterRig {
    bool disc = false;
    CharacterLibrary library;
    CharacterPlacement placement;
};

// Disc when the form's model parsed and a usable idle clip is named.
// Otherwise the caller keeps the placeholder figure.
[[nodiscard]] CharacterRig make_character_rig(AssetRegistry& assets, Form form);

struct CharacterClock {
    Form form = Form::Rabbit;
    int clip = -1;
    std::uint8_t actions = 0;
    bool oneshot = false;
    bool miss = false;
    // A successful obstacle clip cannot be preempted by non-clear animation cues.
    bool successful = false;
    bool started = false;
    double seconds = 0;
};

// How this step should choose an action clip. None keeps the earlier path
// where a press plays the plain clip directly.
enum class ClipMoment : std::uint8_t { None, Clear, Whiff, Wrong };

struct ActionClipCue {
    ClipMoment moment = ClipMoment::None;
    // Clear: the obstacle's actions. Whiff or wrong: the buttons pressed.
    std::uint8_t actions = 0;
};

// Maps one play step onto a clip moment. Does not change the judgment.
[[nodiscard]] ActionClipCue action_clip_cue(const PlayAdvanceResult& step,
                                            const std::vector<PlayHit>& hits,
                                            std::uint8_t edges);

// One line per observed row: name, situation, confidence, then "on" when the
// row is at or above `minimum` and "gated" below it. Stem names only.
[[nodiscard]] std::string character_clip_report(Confidence minimum);

// One line per observed row for one form's clip names: form, name, situation,
// and the clip file the play-time selection picks at `minimum`, or "none".
[[nodiscard]] std::string character_clip_resolve(std::string_view form,
                                                 const std::vector<std::string>& names,
                                                 Confidence minimum);

// Visual only. Does not read or write play timing, score, or judgment.
// A form change returns to that form's idle, except rabbit → super, which
// keeps the current pose and phase. The scribble burst is drawn by the course,
// not by this clock, and that rung does not use it. A clear plays the plain
// clip once, then idle. A whiff plays the _F clip when the library has one,
// otherwise the plain clip. A wrong press plays no action clip. A miss clip
// plays when the library has one.
[[nodiscard]] double character_clip_duration_seconds(const AnmFile& animation);

void character_advance(CharacterClock& clock,
                       const CharacterLibrary& library,
                       const std::vector<AnmFile>& anims,
                       double dt_seconds,
                       std::uint8_t edges,
                       bool missed,
                       Form form,
                       ClipMoment moment = ClipMoment::None,
                       double playback_rate = 1.0);

// Starts the rabbit → super clip when `clip_id` is an index in the form's
// animation list. A negative id does nothing. Already playing that clip is
// left alone, so a later frame does not restart it.
void apply_super_transform_clip(CharacterClock& clock, int clip_id, int clip_count);

struct EyeCullState;

struct DiscFigurePose {
    const TmdModel* model = nullptr;
    const AnmFile* clip = nullptr;
    bool loop = true;
    double seconds = 0;
    CharacterPlacement placement;
    // Remembers which eyes are hidden between frames, for the 45°/40° band.
    // Null decides each frame on its own.
    EyeCullState* eyes = nullptr;
};

// Side view, feet of the placement on `ribbon_y`, then lifted by
// `kFigureRibbonClearancePx`. Body centered on `figure_x`.
// Polygon primitives are filled triangles in `fills` (packet color), sorted
// with the lines by camera-space depth. When `camera` is set, the posed model
// is projected through that ANC camera instead of the flat side view, and
// `disc_projected` is set. False when there is nothing to draw, so the caller
// keeps the placeholder.
[[nodiscard]] bool paint_disc_figure(std::vector<Segment>& out,
                                     const DiscFigurePose& pose,
                                     float figure_x,
                                     float ribbon_y,
                                     std::vector<FilledTriangle>* fills = nullptr,
                                     const AncSample* camera = nullptr,
                                     bool* disc_projected = nullptr);

// Poses at `seconds`. Looping wraps. A one-shot holds the last frame.
[[nodiscard]] std::vector<Pose>
poses_at_time(const AnmFile& animation, std::size_t object_count, double seconds, bool loop);

// Which eye a disc object is. None is the body, the centered head helper, and
// anything that is not part of a left/right eye. An eye is every part that
// sits on it: the fill, a second polygon, and the outline, including parts
// an animation has posed onto the head.
enum class FigureEye : std::uint8_t { None, Left, Right };

struct EyeVisibility {
    bool left = true;
    bool right = true;
};

// Face is world +Z. Yaw is atan2(camera_x, camera_z) with the figure at the
// origin: 0 looks her in the face, positive yaw puts the camera on model +X.
//
// The far eye hides once |yaw| passes kEyeHideFromCameraDegrees (45°) and
// stays hidden until |yaw| comes back below kEyeShowFromCameraDegrees (40°).
// Samples inside that band keep the previous decision, so a few degrees of
// camera or pose wobble cannot toggle it. A one-shot call with no state uses
// the hide angle only: exactly 45° still shows both eyes, and just past 45°
// the far eye drops as a whole.
//
// Facing away, past kEyeHideBothDegrees (150°), both eyes hide. With state,
// once both are hidden they stay hidden until |yaw| is back at or inside
// 150° less kEyeAwaySlackDegrees (4°), so noise around that angle cannot
// toggle them. Between 45° and 150° only the far eye hides. The gameplay
// and menu side view is kEyeSideYawRadians (90°).
inline constexpr float kEyeHideFromCameraDegrees = 45.f;
inline constexpr float kEyeShowFromCameraDegrees = 40.f;
inline constexpr float kEyeBothMaxRadians = 0.7853981633974483f;
inline constexpr float kEyeBothShowRadians = 0.6981317007977318f;
inline constexpr float kEyeSideYawRadians = 1.5707963267948966f;
inline constexpr float kEyeHideBothDegrees = 150.f;
inline constexpr float kEyeAwaySlackDegrees = 4.f;
// 150°. Both eyes hide past this.
inline constexpr float kEyeAwayEnterRadians = 2.6179938779914944f;
// 146°. Once both are hidden, they show again at or inside this.
inline constexpr float kEyeAwayRadians = 2.5481807079117210f;

[[nodiscard]] float figure_yaw_to_camera(float camera_x, float camera_z);

// Strict thresholds, no memory. Exactly the hide angle shows both eyes.
[[nodiscard]] EyeVisibility eye_visibility_for_yaw(float yaw_radians);

// Same facing rules, with the 45°/40° band and the back-half slack above.
// `state` remembers which eyes are hidden and is updated.
[[nodiscard]] EyeVisibility eye_visibility_for_yaw(float yaw_radians, EyeCullState& state);

// One entry per object. Empty of Left/Right when the model has no eye pair.
// Empty `poses` uses bind positions. Otherwise each drawn object is measured
// after its pose, which is where the animation puts the parts.
[[nodiscard]] std::vector<FigureEye> classify_figure_eyes(const TmdModel& model,
                                                          std::span<const Pose> poses = {});

// Yaw hysteresis plus remembered eye primitives. Only an eye fill, a pupil
// posed onto that fill, or an outline line that lies on the fill is stored.
// Head, ears, arms, and any other primitive are never latched.
struct EyeCullState {
    bool left_hidden = false;
    bool right_hidden = false;
    std::vector<std::vector<FigureEye>> primitives;
};

// Hides the primitives of an eye `visibility` rejects: the fill, a pupil
// posed onto it, and outline lines whose bind-pose vertices lie on that
// fill. Lines and polygons that share an object with the eye stay. A pure
// eye object is marked not visible only once every drawable primitive is
// gone. `state` remembers those primitives so pose wobble cannot peel the
// pupil or the outline off the fill. `model` is not modified.
[[nodiscard]] TmdModel cull_figure_eyes(std::vector<Pose>& poses,
                                        const TmdModel& model,
                                        EyeVisibility visibility,
                                        EyeCullState* state = nullptr);

} // namespace oscilline
