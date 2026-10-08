// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Pulls mixed cue samples into the SDL stream.

#include "oscilline/present/sfx_out.hpp"

#include "oscilline/audio/cdda.hpp"

#include <SDL3/SDL.h>
#include <iostream>
#include <utility>
#include <vector>

namespace oscilline {
namespace {

// About 50 ms. Short enough that a hit is heard with the picture, and long
// enough that a frame hitch does not underrun the blip.
constexpr int kQueuedBytes = kCddaRate / 20 * 4;

} // namespace

struct SfxOutput::Impl {
    SfxMixer* mixer = nullptr;
    SDL_AudioStream* stream = nullptr;
    bool audio_inited = false;
    bool paused = false;
    bool gave_up = false;
    std::vector<std::int16_t> scratch;

    ~Impl() {
        if (stream != nullptr) {
            SDL_DestroyAudioStream(stream);
        }
        if (audio_inited) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    void ensure_device() {
        if (stream != nullptr || gave_up) {
            return;
        }
        if (!audio_inited) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                std::cerr << "oscilline: sfx audio init failed (" << SDL_GetError() << ")\n";
                gave_up = true;
                return;
            }
            audio_inited = true;
        }
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16LE;
        spec.channels = 2;
        spec.freq = kCddaRate;
        stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream == nullptr) {
            std::cerr << "oscilline: sfx device unavailable (" << SDL_GetError() << ")\n";
            gave_up = true;
            return;
        }
        SDL_ResumeAudioStreamDevice(stream);
        if (paused) {
            SDL_PauseAudioStreamDevice(stream);
        }
    }
};

SfxOutput::SfxOutput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SfxOutput::SfxOutput(SfxOutput&&) noexcept = default;
SfxOutput& SfxOutput::operator=(SfxOutput&&) noexcept = default;
SfxOutput::~SfxOutput() = default;

SfxOutput SfxOutput::open(SfxMixer& mixer) {
    auto impl = std::make_unique<Impl>();
    impl->mixer = &mixer;
    return SfxOutput{std::move(impl)};
}

void SfxOutput::set_paused(bool paused) {
    if (impl_->paused == paused) {
        return;
    }
    impl_->paused = paused;
    if (impl_->stream == nullptr) {
        return;
    }
    if (paused) {
        SDL_PauseAudioStreamDevice(impl_->stream);
    } else {
        SDL_ResumeAudioStreamDevice(impl_->stream);
    }
}

void SfxOutput::discard() {
    if (impl_->stream != nullptr) {
        SDL_ClearAudioStream(impl_->stream);
    }
}

void SfxOutput::pump() {
    if (impl_->paused || impl_->mixer == nullptr) {
        return;
    }
    const bool work = impl_->stream != nullptr || impl_->mixer->pending() ||
                      impl_->mixer->sequence_active() || impl_->mixer->active_voices() > 0;
    if (!work) {
        return;
    }
    impl_->ensure_device();
    int frames = kCddaRate / 20;
    if (impl_->stream != nullptr) {
        const int queued = SDL_GetAudioStreamQueued(impl_->stream);
        if (queued >= kQueuedBytes) {
            return;
        }
        int bytes = queued < 0 ? kQueuedBytes : kQueuedBytes - queued;
        bytes -= bytes % 4;
        if (bytes < 4) {
            return;
        }
        frames = bytes / 4;
    }
    impl_->scratch.assign(static_cast<std::size_t>(frames) * 2u, 0);
    impl_->mixer->mix(impl_->scratch, frames);
    if (impl_->stream == nullptr) {
        return;
    }
    SDL_PutAudioStreamData(impl_->stream, impl_->scratch.data(), frames * 4);
}

bool SfxOutput::device_open() const {
    return impl_->stream != nullptr;
}

} // namespace oscilline
