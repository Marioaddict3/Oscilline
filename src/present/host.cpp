// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Window, logical presentation, and the input edge set.

#include "oscilline/present/host.hpp"

#include "oscilline/settings.hpp"
#include "oscilline/version.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

struct DeviceBits {
    bool down[static_cast<std::size_t>(Key::Count)]{};
};

enum class Device {
    Keyboard,
    Pad,
};

struct OpenPad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* pad = nullptr;
};

struct DiscPick {
    std::mutex mu;
    bool done = false;
    std::string path;
    std::string error;
};

struct DiscHolder {
    std::shared_ptr<DiscPick> pick;
};

void on_disc_file(void* userdata, const char* const* filelist, int filter) {
    (void)filter;
    const std::unique_ptr<DiscHolder> holder(static_cast<DiscHolder*>(userdata));
    if (!holder || !holder->pick) {
        return;
    }
    std::lock_guard lock(holder->pick->mu);
    holder->pick->done = true;
    holder->pick->path.clear();
    holder->pick->error.clear();
    if (filelist == nullptr) {
        const char* message = SDL_GetError();
        holder->pick->error =
            message != nullptr && message[0] != '\0' ? message : "file dialog failed";
        return;
    }
    if (filelist[0] != nullptr) {
        holder->pick->path = filelist[0];
    }
}

static_assert(static_cast<int>(PadButton::South) == SDL_GAMEPAD_BUTTON_SOUTH);
static_assert(static_cast<int>(PadButton::LeftShoulder) == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
static_assert(static_cast<int>(PadButton::DpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(static_cast<int>(Key::Pit) - static_cast<int>(Key::Block) + 1 == kGameActionCount);

bool navigation(Key key) {
    return key == Key::Left || key == Key::Right || key == Key::Up || key == Key::Down;
}

void set_logical_presentation(SDL_Renderer* renderer, int width) {
    SDL_SetRenderLogicalPresentation(
        renderer, width, kLogicalHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    set_logical_width(width);
}

} // namespace

struct Host::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::vector<OpenPad> pads;
    std::vector<SDL_Vertex> gpu;
    DeviceBits keyboard;
    DeviceBits pad;
    std::unordered_set<SDL_Keycode> held_keys;
    HostInput input;
    ControlBindings controls;
    std::array<SDL_Keycode, kGameActionCount> action_keys{};
    bool pad_was_down[static_cast<std::size_t>(PadButton::Count)]{};
    FixedStep clock{1.0 / 60.0};
    int frame_limit = -1;
    int presented = 0;
    int default_width = 960;
    int default_height = 720;
    int window_scale = 0;
    bool fullscreen = false;
    ViewportAspect aspect_ratio = ViewportAspect::FourThree;
    int logical_viewport_width = kWidth;
    bool quit = false;
    bool dialog_open = false;
    std::shared_ptr<DiscPick> pick;
    std::shared_ptr<DiscPick> music_pick;
    Uint64 frame_start = 0;
    bool classic = false;
    SDL_Texture* classic_target = nullptr;
    int classic_width = 0;

    // The low-resolution frame for Classic, sized for the current logical width.
    SDL_Texture* classic_frame() {
        const int width =
            std::max(1,
                     static_cast<int>(std::lround(static_cast<double>(logical_width()) *
                                                  kClassicHeight / kLogicalHeight)));
        if (classic_target != nullptr && classic_width == width) {
            return classic_target;
        }
        if (classic_target != nullptr) {
            SDL_DestroyTexture(classic_target);
        }
        classic_target = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, kClassicHeight);
        classic_width = classic_target != nullptr ? width : 0;
        if (classic_target != nullptr) {
            SDL_SetTextureScaleMode(classic_target, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(classic_target, SDL_BLENDMODE_NONE);
        }
        return classic_target;
    }

    void update_logical_viewport() {
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height)) {
            return;
        }
        const int width = logical_width_for_aspect(aspect_ratio, output_width, output_height);
        if (width != logical_viewport_width) {
            logical_viewport_width = width;
            set_logical_presentation(renderer, width);
        }
    }

    ~Impl() {
        for (OpenPad& open : pads) {
            SDL_CloseGamepad(open.pad);
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }

    void refresh(Key key, bool repeat) {
        const std::size_t index = static_cast<std::size_t>(key);
        const bool now = keyboard.down[index] || pad.down[index];
        if ((now && !input.down[index]) || (repeat && now && navigation(key))) {
            input.pressed[index] = true;
        }
        input.down[index] = now;
    }

    void set_key(Device device, Key key, bool down, bool repeat) {
        DeviceBits& bits = device == Device::Keyboard ? keyboard : pad;
        bits.down[static_cast<std::size_t>(key)] = down;
        refresh(key, repeat);
    }

    void open_pad(SDL_JoystickID id) {
        for (const OpenPad& open : pads) {
            if (open.id == id) {
                return;
            }
        }
        SDL_Gamepad* pad_handle = SDL_OpenGamepad(id);
        if (pad_handle == nullptr) {
            return;
        }
        pads.push_back(OpenPad{id, pad_handle});
    }

    void close_pad(SDL_JoystickID id) {
        for (auto it = pads.begin(); it != pads.end(); ++it) {
            if (it->id == id) {
                SDL_CloseGamepad(it->pad);
                pads.erase(it);
                return;
            }
        }
    }

    void poll_pads() {
        const auto held_button = [&](SDL_GamepadButton button) {
            for (const OpenPad& open : pads) {
                if (SDL_GetGamepadButton(open.pad, button)) {
                    return true;
                }
            }
            return false;
        };
        const auto assign = [&](SDL_GamepadButton button, Key key) {
            set_key(Device::Pad, key, held_button(button), false);
        };
        assign(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Key::LeftShoulder);
        assign(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Key::RightShoulder);
        const bool face = held_button(SDL_GAMEPAD_BUTTON_SOUTH);
        set_key(Device::Pad, Key::Face, face, false);
        set_key(Device::Pad, Key::Confirm, face, false);
        assign(SDL_GAMEPAD_BUTTON_DPAD_DOWN, Key::Down);
        assign(SDL_GAMEPAD_BUTTON_DPAD_UP, Key::Up);
        assign(SDL_GAMEPAD_BUTTON_DPAD_LEFT, Key::Left);
        assign(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Key::Right);
        assign(SDL_GAMEPAD_BUTTON_START, Key::Start);
        // North is triangle. It cancels, the same as the pad's back button.
        const bool back =
            held_button(SDL_GAMEPAD_BUTTON_BACK) || held_button(SDL_GAMEPAD_BUTTON_NORTH);
        set_key(Device::Pad, Key::Back, back, false);
        for (int i = 0; i < kGameActionCount; ++i) {
            const auto button =
                static_cast<SDL_GamepadButton>(controls.pad[static_cast<std::size_t>(i)]);
            set_key(Device::Pad,
                    static_cast<Key>(static_cast<int>(Key::Block) + i),
                    held_button(button),
                    false);
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(PadButton::Count); ++i) {
            const bool now = held_button(static_cast<SDL_GamepadButton>(i));
            if (now && !pad_was_down[i]) {
                if (input.pad_button_pressed < 0) {
                    input.pad_button_pressed = static_cast<int>(i);
                }
                input.pad_active = true;
            }
            pad_was_down[i] = now;
        }
    }

    void apply_controls(const ControlBindings& next) {
        controls = next;
        for (std::size_t i = 0; i < action_keys.size(); ++i) {
            action_keys[i] = SDL_GetKeyFromName(controls.keys[i].c_str());
        }
        rebuild_keyboard(SDLK_UNKNOWN);
    }

    // Space waves and Down pits while those actions keep their default keys,
    // unless another action is bound to the alias.
    bool alias_free(SDL_Keycode alias, GameAction action, std::string_view default_key) const {
        const auto index = static_cast<std::size_t>(action);
        if (!key_names_equal(controls.keys[index], default_key)) {
            return false;
        }
        for (const SDL_Keycode code : action_keys) {
            if (code == alias) {
                return false;
            }
        }
        return true;
    }

    void rebuild_keyboard(SDL_Keycode repeated) {
        const auto has = [&](SDL_Keycode code) { return held_keys.find(code) != held_keys.end(); };
        const auto assign = [&](Key key, bool down, std::initializer_list<SDL_Keycode> codes) {
            bool repeat = false;
            for (const SDL_Keycode code : codes) {
                if (code == repeated) {
                    repeat = true;
                }
            }
            set_key(Device::Keyboard, key, down, repeat);
        };
        assign(Key::LeftShoulder, has(SDLK_Q), {SDLK_Q});
        assign(Key::RightShoulder, has(SDLK_E), {SDLK_E});
        assign(Key::Face, has(SDLK_SPACE) || has(SDLK_X), {SDLK_SPACE, SDLK_X});
        assign(Key::Down, has(SDLK_DOWN) || has(SDLK_S), {SDLK_DOWN, SDLK_S});
        assign(Key::Up, has(SDLK_UP) || has(SDLK_W), {SDLK_UP, SDLK_W});
        assign(Key::Left, has(SDLK_LEFT) || has(SDLK_A), {SDLK_LEFT, SDLK_A});
        assign(Key::Right, has(SDLK_RIGHT) || has(SDLK_D), {SDLK_RIGHT, SDLK_D});
        assign(Key::Confirm, has(SDLK_RETURN) || has(SDLK_KP_ENTER), {SDLK_RETURN, SDLK_KP_ENTER});
        assign(Key::Start, has(SDLK_P), {SDLK_P});
        assign(Key::Back, has(SDLK_ESCAPE), {SDLK_ESCAPE});
        assign(Key::LookUp, has(SDLK_R), {SDLK_R});
        assign(Key::LookDown, has(SDLK_F), {SDLK_F});
        assign(Key::ZoomIn,
               has(SDLK_EQUALS) || has(SDLK_PLUS) || has(SDLK_KP_PLUS),
               {SDLK_EQUALS, SDLK_PLUS, SDLK_KP_PLUS});
        assign(Key::ZoomOut, has(SDLK_MINUS) || has(SDLK_KP_MINUS), {SDLK_MINUS, SDLK_KP_MINUS});
        assign(Key::Sfx, has(SDLK_TAB), {SDLK_TAB});
        for (std::size_t i = 0; i < action_keys.size(); ++i) {
            const auto action = static_cast<GameAction>(i);
            const SDL_Keycode code = action_keys[i];
            bool down = code != SDLK_UNKNOWN && has(code);
            if (action == GameAction::Wave && alias_free(SDLK_SPACE, action, "X")) {
                down = down || has(SDLK_SPACE);
            } else if (action == GameAction::Pit && alias_free(SDLK_DOWN, action, "S")) {
                down = down || has(SDLK_DOWN);
            }
            set_key(
                Device::Keyboard, static_cast<Key>(static_cast<int>(Key::Block) + i), down, false);
        }
    }

    void on_motion(const SDL_MouseMotionEvent& motion) {
        // Record the drag only. Do not grab the cursor: this host is also the
        // game window, and a click there must not hide the pointer.
        if ((motion.state & SDL_BUTTON_LMASK) == 0) {
            return;
        }
        input.drag_x += motion.xrel;
        input.drag_y += motion.yrel;
    }

    void on_wheel(const SDL_MouseWheelEvent& wheel) {
        float y = wheel.y;
        if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            y = -y;
        }
        input.wheel_y += y;
    }

    void on_key(SDL_Keycode code, bool down, bool repeat) {
        if (down && !repeat) {
            if (input.key_name_pressed.empty()) {
                const char* name = SDL_GetKeyName(code);
                input.key_name_pressed = name != nullptr ? name : "";
            }
            input.pad_active = false;
        }
        if (down) {
            held_keys.insert(code);
        } else {
            held_keys.erase(code);
        }
        rebuild_keyboard(repeat && down ? code : SDLK_UNKNOWN);
    }

    void handle(const SDL_Event& event) {
        if (event.type == SDL_EVENT_QUIT) {
            quit = true;
            return;
        }
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            on_key(event.key.key, event.key.down, event.key.repeat);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            on_motion(event.motion);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            on_wheel(event.wheel);
            return;
        }
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            open_pad(event.gdevice.which);
            return;
        }
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            close_pad(event.gdevice.which);
        }
    }
};

Host::Host(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Host::Host(Host&&) noexcept = default;
Host& Host::operator=(Host&&) noexcept = default;
Host::~Host() = default;

Result<Host> Host::open(Config config) {
    // Desktops match the window to the installed desktop entry and icon by this ID.
    SDL_SetAppMetadata("Oscilline", version_string(), "io.github.marioaddict3.Oscilline");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return Result<Host>::failure(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    int window_width = config.window_width;
    int window_height = config.window_height;
    if (config.aspect_ratio == ViewportAspect::SixteenNine && window_width == 960 &&
        window_height == 720) {
        window_width = 1280;
    }
    SDL_Window* window = SDL_CreateWindow(config.title.c_str(),
                                          window_width,
                                          window_height,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        const std::string message = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return Result<Host>::failure(message);
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, "software");
    }
    if (renderer == nullptr) {
        const std::string message = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return Result<Host>::failure(message);
    }
    int output_width = window_width;
    int output_height = window_height;
    SDL_GetRenderOutputSize(renderer, &output_width, &output_height);
    const int logical_viewport_width =
        logical_width_for_aspect(config.aspect_ratio, output_width, output_height);
    set_logical_presentation(renderer, logical_viewport_width);
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    auto impl = std::make_unique<Impl>();
    impl->window = window;
    impl->renderer = renderer;
    impl->frame_limit = config.frame_limit;
    impl->default_width = window_width;
    impl->default_height = window_height;
    impl->aspect_ratio = config.aspect_ratio;
    impl->logical_viewport_width = logical_viewport_width;
    impl->apply_controls(ControlBindings{});
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int i = 0; i < count; ++i) {
            impl->open_pad(ids[i]);
        }
        SDL_free(ids);
    }
    return Result<Host>::success(Host{std::move(impl)});
}

bool Host::begin_frame() {
    if (impl_->quit) {
        return false;
    }
    if (impl_->frame_limit >= 0 && impl_->presented >= impl_->frame_limit) {
        return false;
    }
    impl_->frame_start = SDL_GetTicksNS();
    impl_->input.pressed.fill(false);
    impl_->input.drag_x = 0.f;
    impl_->input.drag_y = 0.f;
    impl_->input.wheel_y = 0.f;
    impl_->input.key_name_pressed.clear();
    impl_->input.pad_button_pressed = -1;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        impl_->handle(event);
    }
    impl_->poll_pads();
    if (impl_->aspect_ratio == ViewportAspect::Unrestricted) {
        impl_->update_logical_viewport();
    }
    return !impl_->quit;
}

void Host::end_frame(const TriangleList& triangles, std::span<const TextGlyph> text) {
    SDL_Texture* classic = impl_->classic ? impl_->classic_frame() : nullptr;
    if (classic != nullptr) {
        // Draw the logical view into the 286-line frame. Geometry keeps its
        // logical coordinates; the render scale maps them onto the frame.
        SDL_SetRenderTarget(impl_->renderer, classic);
        const float scale = static_cast<float>(kClassicHeight) / static_cast<float>(kHeight);
        SDL_SetRenderScale(impl_->renderer, scale, scale);
    }
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    SDL_RenderClear(impl_->renderer);

    const TriangleList drawn = drop_degenerate_triangles(
        triangles, static_cast<float>(logical_width()), static_cast<float>(kHeight));
    const std::size_t count = drawn.vertices.size();
    if (count >= 3 && drawn.vertices.data() != nullptr) {
        impl_->gpu.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const Vertex& source = drawn.vertices[i];
            SDL_Vertex& dest = impl_->gpu[i];
            dest.position.x = source.x;
            dest.position.y = source.y;
            dest.color.r = source.r;
            dest.color.g = source.g;
            dest.color.b = source.b;
            dest.color.a = source.a;
            dest.tex_coord.x = 0;
            dest.tex_coord.y = 0;
        }
        SDL_RenderGeometry(
            impl_->renderer, nullptr, impl_->gpu.data(), static_cast<int>(count), nullptr, 0);
    }

    if (classic != nullptr) {
        SDL_SetRenderScale(impl_->renderer, 1.f, 1.f);
        SDL_SetRenderTarget(impl_->renderer, nullptr);
        SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
        SDL_RenderClear(impl_->renderer);
        const SDL_FRect whole{
            0.f, 0.f, static_cast<float>(logical_width()), static_cast<float>(kHeight)};
        SDL_RenderTexture(impl_->renderer, classic, nullptr, &whole);
    }

    SDL_SetRenderDrawColorFloat(impl_->renderer, 1.f, 1.f, 1.f, 1.f);
    for (const TextGlyph& glyph : text) {
        if (!glyph.text.empty()) {
            const float scale_x = glyph.scale * glyph.horizontal_scale;
            const float scale_y = glyph.scale;
            if (std::isfinite(scale_x) && scale_x > 0.f && std::isfinite(scale_y) &&
                scale_y > 0.f && (scale_x != 1.f || scale_y != 1.f)) {
                SDL_SetRenderScale(impl_->renderer, scale_x, scale_y);
                const float text_x = glyph.x / scale_x;
                const float text_y = glyph.y / scale_y;
                SDL_RenderDebugText(impl_->renderer, text_x, text_y, glyph.text.c_str());
                SDL_SetRenderScale(impl_->renderer, 1.f, 1.f);
            } else {
                SDL_RenderDebugText(impl_->renderer, glyph.x, glyph.y, glyph.text.c_str());
            }
        }
    }
    SDL_RenderPresent(impl_->renderer);
    ++impl_->presented;

    if (impl_->frame_limit < 0) {
        constexpr Uint64 kFrameNs = 1000000000ull / 60ull;
        const Uint64 elapsed = SDL_GetTicksNS() - impl_->frame_start;
        if (elapsed < kFrameNs) {
            SDL_DelayNS(kFrameNs - elapsed);
        }
    }
}

void Host::set_controls(const ControlBindings& controls) {
    impl_->apply_controls(controls);
}

const ControlBindings& Host::controls() const {
    return impl_->controls;
}

const HostInput& Host::input() const {
    return impl_->input;
}

int Host::sim_steps() {
    const double now = static_cast<double>(SDL_GetTicksNS()) / 1000000000.0;
    return impl_->clock.advance(now);
}

double Host::alpha() const {
    return impl_->clock.alpha();
}

double Host::step_seconds() const {
    return impl_->clock.step_seconds();
}

int Host::frames_presented() const {
    return impl_->presented;
}

int Host::max_window_scale() const {
    int max_scale = 1;
    SDL_DisplayID display = SDL_GetDisplayForWindow(impl_->window);
    if (display == 0) {
        display = SDL_GetPrimaryDisplay();
    }
    SDL_Rect bounds{};
    if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds) && bounds.w > 0 &&
        bounds.h > 0) {
        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(impl_->window, &window_width, &window_height);
        const int scale_width = impl_->aspect_ratio == ViewportAspect::Unrestricted &&
                                        window_width > 0 && window_height > 0
                                    ? (kHeight * window_width) / window_height
                                    : logical_width_for_aspect(impl_->aspect_ratio);
        while (max_scale < kWindowScaleMax && (max_scale + 1) * scale_width <= bounds.w &&
               (max_scale + 1) * kHeight <= bounds.h) {
            ++max_scale;
        }
        return max_scale;
    }
    // No display size: still offer a few integer steps.
    return 4;
}

int Host::set_window_scale(int scale) {
    if (scale < 0) {
        scale = 0;
    }
    if (scale > max_window_scale()) {
        scale = max_window_scale();
    }
    impl_->window_scale = scale;
    if (!impl_->fullscreen) {
        if (scale <= 0) {
            SDL_SetWindowSize(impl_->window, impl_->default_width, impl_->default_height);
        } else {
            int width = logical_width_for_aspect(impl_->aspect_ratio) * scale;
            if (impl_->aspect_ratio == ViewportAspect::Unrestricted) {
                int current_width = 0;
                int current_height = 0;
                SDL_GetWindowSize(impl_->window, &current_width, &current_height);
                if (current_width > 0 && current_height > 0) {
                    width = (kHeight * scale * current_width) / current_height;
                }
            }
            SDL_SetWindowSize(impl_->window, width, kHeight * scale);
        }
    }
    return scale;
}

void Host::set_aspect_ratio(ViewportAspect aspect) {
    if (impl_->aspect_ratio == aspect) {
        return;
    }
    impl_->aspect_ratio = aspect;
    int output_width = 0;
    int output_height = 0;
    SDL_GetRenderOutputSize(impl_->renderer, &output_width, &output_height);
    impl_->logical_viewport_width = logical_width_for_aspect(aspect, output_width, output_height);
    set_logical_presentation(impl_->renderer, impl_->logical_viewport_width);
    if (aspect != ViewportAspect::Unrestricted) {
        impl_->default_width = aspect == ViewportAspect::SixteenNine ? 1280 : 960;
        impl_->default_height = 720;
    }
    if (!impl_->fullscreen) {
        if (impl_->window_scale > 0) {
            set_window_scale(impl_->window_scale);
        } else if (aspect != ViewportAspect::Unrestricted) {
            SDL_SetWindowSize(impl_->window, impl_->default_width, impl_->default_height);
        }
    }
}

void Host::set_classic_resolution(bool classic) {
    impl_->classic = classic;
}

void Host::set_fullscreen(bool on) {
    if (impl_->fullscreen == on) {
        return;
    }
    impl_->fullscreen = on;
    SDL_SetWindowFullscreen(impl_->window, on);
    if (!on) {
        set_window_scale(impl_->window_scale);
    }
}

bool Host::fullscreen() const {
    return impl_->fullscreen;
}

void Host::choose_disc_file() {
    if (impl_->dialog_open) {
        return;
    }
    if (!impl_->pick) {
        impl_->pick = std::make_shared<DiscPick>();
    }
    {
        std::lock_guard lock(impl_->pick->mu);
        impl_->pick->done = false;
        impl_->pick->path.clear();
        impl_->pick->error.clear();
    }
    impl_->dialog_open = true;
    static const SDL_DialogFileFilter kFilters[] = {
        {"Disc image", "cue;bin;iso"},
        {"All files", "*"},
    };
    auto* holder = new DiscHolder;
    holder->pick = impl_->pick;
    SDL_ShowOpenFileDialog(on_disc_file, holder, impl_->window, kFilters, 2, nullptr, false);
}

void Host::choose_music_file() {
    if (impl_->dialog_open) {
        return;
    }
    if (!impl_->music_pick) {
        impl_->music_pick = std::make_shared<DiscPick>();
    }
    {
        std::lock_guard lock(impl_->music_pick->mu);
        impl_->music_pick->done = false;
        impl_->music_pick->path.clear();
        impl_->music_pick->error.clear();
    }
    impl_->dialog_open = true;
    static const SDL_DialogFileFilter kFilters[] = {
        {"Music", "wav;flac;mp3"},
        {"All files", "*"},
    };
    auto* holder = new DiscHolder;
    holder->pick = impl_->music_pick;
    SDL_ShowOpenFileDialog(on_disc_file, holder, impl_->window, kFilters, 2, nullptr, false);
}

bool Host::take_music_file(std::string& path, std::string& error) {
    if (!impl_->music_pick) {
        return false;
    }
    std::lock_guard lock(impl_->music_pick->mu);
    if (!impl_->music_pick->done) {
        return false;
    }
    path = std::move(impl_->music_pick->path);
    error = std::move(impl_->music_pick->error);
    impl_->music_pick->done = false;
    impl_->music_pick->path.clear();
    impl_->music_pick->error.clear();
    impl_->dialog_open = false;
    return true;
}

bool Host::take_disc_file(std::string& path, std::string& error) {
    if (!impl_->pick) {
        return false;
    }
    std::lock_guard lock(impl_->pick->mu);
    if (!impl_->pick->done) {
        return false;
    }
    path = std::move(impl_->pick->path);
    error = std::move(impl_->pick->error);
    impl_->pick->done = false;
    impl_->pick->path.clear();
    impl_->pick->error.clear();
    impl_->dialog_open = false;
    return true;
}

} // namespace oscilline
