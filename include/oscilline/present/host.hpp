// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// SDL window, input, and one presented frame.

#pragma once

#include "oscilline/controls.hpp"
#include "oscilline/render/clock.hpp"
#include "oscilline/render/stroke.hpp"
#include "oscilline/render/viewport.hpp"
#include "oscilline/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace oscilline {

// Buttons shared by the keyboard and a gamepad. Shoulders and the south face
// button follow the pad; the keyboard equivalents are documented on the tools.
// Look and zoom are keyboard-only. The model viewer uses them to orbit.
enum class Key : std::uint8_t {
    LeftShoulder,
    RightShoulder,
    Face,
    Down,
    Up,
    Left,
    Right,
    Start,
    Confirm,
    Back,
    LookUp,
    LookDown,
    ZoomIn,
    ZoomOut,
    // Keyboard-only. The model viewer uses Tab to open the SFX browser.
    Sfx,
    // Gameplay actions from the player's ControlBindings. The keys above stay
    // fixed for menus and the viewer.
    Block,
    Loop,
    Wave,
    Pit,
    Count,
};

struct HostInput {
    std::array<bool, static_cast<std::size_t>(Key::Count)> down{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> pressed{};
    // Primary-button drag since the previous frame, in window pixels.
    // Stays zero when the button is up, including a headless --frames run.
    float drag_x = 0.f;
    float drag_y = 0.f;
    // Wheel since the previous frame. Positive is the traditional wheel-up
    // direction. Stays zero when nothing scrolls.
    float wheel_y = 0.f;
    // First new key (SDL key name) and pad button this frame, for rebinding.
    // Empty and -1 when there was none. Key repeats do not count.
    std::string key_name_pressed;
    int pad_button_pressed = -1;
    // True after a pad button press, false again after a key press.
    bool pad_active = false;
};

struct TextGlyph {
    float x = 0;
    float y = 0;
    std::string text;
    float scale = 1.f;
    float horizontal_scale = 1.f;
};

[[nodiscard]] inline bool held(const HostInput& input, Key key) {
    return input.down[static_cast<std::size_t>(key)];
}

[[nodiscard]] inline bool pressed(const HostInput& input, Key key) {
    return input.pressed[static_cast<std::size_t>(key)];
}

// SDL3 window with an aspect-aware logical view, HiDPI, and a 60 Hz
// fixed step. Drawing is white-on-black triangle geometry plus plain debug text.
class Host {
  public:
    struct Config {
        std::string title = "Oscilline";
        int frame_limit = -1;
        int window_width = 960;
        int window_height = 720;
        ViewportAspect aspect_ratio = ViewportAspect::FourThree;
    };

    static Result<Host> open(Config config);

    Host(Host&&) noexcept;
    Host& operator=(Host&&) noexcept;
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // False when the window closes or the frame limit has already been drawn.
    [[nodiscard]] bool begin_frame();

    void end_frame(const TriangleList& triangles, std::span<const TextGlyph> text);

    [[nodiscard]] const HostInput& input() const;

    // How many 60 Hz steps elapsed since the previous call. Call once a frame.
    [[nodiscard]] int sim_steps();

    [[nodiscard]] double alpha() const;

    [[nodiscard]] double step_seconds() const;

    [[nodiscard]] int frames_presented() const;

    // Largest integer scale that fits the selected window aspect. At least 1.
    [[nodiscard]] int max_window_scale() const;

    // 0 restores the window size from Config. A positive scale follows the
    // selected aspect ratio, clamped to max_window_scale.
    int set_window_scale(int scale);

    void set_fullscreen(bool on);

    // Applies the viewport aspect ratio and resizes the window when windowed.
    void set_aspect_ratio(ViewportAspect aspect);

    // Classic draws each frame at kClassicHeight lines, with the width the
    // aspect ratio gives at square pixels, then scales it up with hard pixel
    // edges. Debug text is drawn after that, at full resolution, so it stays
    // readable. False (HD) draws at the window's own resolution.
    void set_classic_resolution(bool classic);

    [[nodiscard]] bool fullscreen() const;

    // Keyboard keys and pad buttons for Key::Block, Loop, Wave, and Pit.
    void set_controls(const ControlBindings& controls);
    [[nodiscard]] const ControlBindings& controls() const;

    // Asynchronous disc-image picker. The result is collected with take_disc_file.
    void choose_disc_file();

    // True when a picker has finished. `path` is empty if the picker was canceled.
    [[nodiscard]] bool take_disc_file(std::string& path, std::string& error);

    // Asynchronous picker for a wav, flac, or mp3 file.
    void choose_music_file();

    [[nodiscard]] bool take_music_file(std::string& path, std::string& error);

    static constexpr int kWidth = kLogicalWidth;
    static constexpr int kHeight = kLogicalHeight;
    // The original game's frame height, used by the Classic resolution.
    static constexpr int kClassicHeight = 286;

  private:
    struct Impl;
    explicit Host(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace oscilline
