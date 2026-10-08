// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Device callback and the consumed-sample clock, including the prelude.

#include "oscilline/present/cdda_out.hpp"

#include "oscilline/audio/cdda.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>
#include <utility>

namespace oscilline {
namespace {

// The stream clock leads the speaker by the device buffer. 0 until a disc
// shows the picture ahead of the music; raise it to delay the course clock.
constexpr int kPlaybackLeadMs = 0;
constexpr int kQueuedBytes = kCddaRate * 4;

} // namespace

struct CddaOutput::Impl {
    SDL_AudioStream* stream = nullptr;
    bool audio_inited = false;
    bool paused = false;
    bool started = true;
    int start_delay_ms = 0;
    std::vector<std::int16_t> pcm;
    int frames = 0;
    std::int64_t submitted = 0;
    Uint64 origin = 0;
    Uint64 paused_total = 0;
    Uint64 pause_started = 0;
    int music_gain_q15 = 32767;
    bool overrun = false;
    Uint64 overrun_origin = 0;
    std::int64_t overrun_base_ms = 0;
    Uint64 overrun_paused_total = 0;
    Uint64 overrun_pause_started = 0;

    ~Impl() {
        if (stream != nullptr) {
            SDL_DestroyAudioStream(stream);
        }
        if (audio_inited) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    void pump() {
        if (stream == nullptr || paused || !started) {
            return;
        }
        const int queued = SDL_GetAudioStreamQueued(stream);
        if (queued < 0 || queued >= kQueuedBytes) {
            return;
        }
        int bytes = kQueuedBytes - queued;
        bytes -= bytes % 4;
        const std::int64_t remain = static_cast<std::int64_t>(frames) - submitted;
        if (remain <= 0 || bytes < 4) {
            return;
        }
        int frames_now = bytes / 4;
        if (static_cast<std::int64_t>(frames_now) > remain) {
            frames_now = static_cast<int>(remain);
        }
        const std::int16_t* src = pcm.data() + static_cast<std::size_t>(submitted) * 2u;
        if (!SDL_PutAudioStreamData(stream, src, frames_now * 4)) {
            return;
        }
        submitted += frames_now;
    }
};

CddaOutput::CddaOutput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    // Device setup must not consume part of the measured prelude.
    if (impl_->start_delay_ms > 0) {
        impl_->origin = SDL_GetTicks();
    }
}

CddaOutput::CddaOutput(CddaOutput&&) noexcept = default;
CddaOutput& CddaOutput::operator=(CddaOutput&&) noexcept = default;
CddaOutput::~CddaOutput() = default;

CddaOutput CddaOutput::open(Config config) {
    auto impl = std::make_unique<Impl>();
    impl->origin = SDL_GetTicks();
    impl->start_delay_ms = std::max(config.start_delay_ms, 0);
    impl->started = impl->start_delay_ms == 0;
    const std::size_t samples = config.interleaved.size() / 2u;
    if (config.frames < 0 || static_cast<std::size_t>(config.frames) > samples) {
        config.frames = static_cast<int>(samples);
    }
    impl->frames = config.frames;
    impl->pcm = std::move(config.interleaved);

    if (impl->frames <= 0) {
        std::cerr << "oscilline: no CD-DA samples; timing follows the system clock\n";
        return CddaOutput{std::move(impl)};
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::cerr << "oscilline: audio init failed (" << SDL_GetError()
                  << "); timing follows the system clock\n";
        return CddaOutput{std::move(impl)};
    }
    impl->audio_inited = true;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = kCddaRate;
    impl->stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (impl->stream == nullptr) {
        std::cerr << "oscilline: audio device unavailable (" << SDL_GetError()
                  << "); timing follows the system clock\n";
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        impl->audio_inited = false;
        return CddaOutput{std::move(impl)};
    }
    impl->pump();
    if (impl->started) {
        SDL_ResumeAudioStreamDevice(impl->stream);
    }
    return CddaOutput{std::move(impl)};
}

void CddaOutput::set_music_gain(int gain_q15) {
    if (gain_q15 < 0) {
        gain_q15 = 0;
    }
    if (gain_q15 > 32767) {
        gain_q15 = 32767;
    }
    if (impl_->stream == nullptr || impl_->music_gain_q15 == gain_q15) {
        impl_->music_gain_q15 = gain_q15;
        return;
    }
    impl_->music_gain_q15 = gain_q15;
    const float gain = static_cast<float>(gain_q15) / 32767.f;
    SDL_SetAudioStreamGain(impl_->stream, gain);
}

void CddaOutput::set_paused(bool paused) {
    if (impl_->paused == paused) {
        return;
    }
    const Uint64 now = SDL_GetTicks();
    impl_->paused = paused;
    if (impl_->stream != nullptr) {
        if (paused) {
            SDL_PauseAudioStreamDevice(impl_->stream);
        } else if (impl_->started) {
            SDL_ResumeAudioStreamDevice(impl_->stream);
        }
    }
    if (paused) {
        impl_->pause_started = now;
    } else {
        impl_->paused_total += now - impl_->pause_started;
    }
    if (impl_->overrun) {
        if (paused) {
            impl_->overrun_pause_started = now;
        } else {
            impl_->overrun_paused_total += now - impl_->overrun_pause_started;
        }
    }
}

std::int64_t CddaOutput::position_ms() {
    const Uint64 wall_now = SDL_GetTicks();
    const Uint64 frozen = impl_->paused ? wall_now - impl_->pause_started : 0;
    const Uint64 elapsed = wall_now - impl_->origin - impl_->paused_total - frozen;
    if (!impl_->started) {
        if (elapsed < static_cast<Uint64>(impl_->start_delay_ms)) {
            return static_cast<std::int64_t>(elapsed) - impl_->start_delay_ms;
        }
        impl_->started = true;
        impl_->pump();
        if (impl_->stream != nullptr && !impl_->paused) {
            SDL_ResumeAudioStreamDevice(impl_->stream);
        }
    }
    if (impl_->stream != nullptr) {
        impl_->pump();
        int queued = SDL_GetAudioStreamQueued(impl_->stream);
        if (queued < 0) {
            queued = 0;
        }
        std::int64_t played = impl_->submitted - static_cast<std::int64_t>(queued) / 4;
        if (played < 0) {
            played = 0;
        }
        std::int64_t ms = played * 1000 / static_cast<std::int64_t>(kCddaRate);
        ms -= kPlaybackLeadMs;
        if (ms < 0) {
            ms = 0;
        }
        const bool drained = impl_->submitted >= impl_->frames && queued == 0;
        if (!drained) {
            return ms;
        }
        const Uint64 now = SDL_GetTicks();
        if (!impl_->overrun) {
            impl_->overrun = true;
            impl_->overrun_origin = now;
            impl_->overrun_base_ms = ms;
            impl_->overrun_paused_total = 0;
            impl_->overrun_pause_started = impl_->paused ? now : 0;
        }
        const Uint64 overrun_frozen = impl_->paused ? now - impl_->overrun_pause_started : 0;
        const Uint64 overrun_elapsed =
            now - impl_->overrun_origin - impl_->overrun_paused_total - overrun_frozen;
        return impl_->overrun_base_ms + static_cast<std::int64_t>(overrun_elapsed);
    }
    return static_cast<std::int64_t>(elapsed) - impl_->start_delay_ms;
}

bool CddaOutput::device_open() const {
    return impl_->stream != nullptr;
}

} // namespace oscilline
