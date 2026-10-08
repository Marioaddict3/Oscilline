// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Mixes event cues into the music stream.

#pragma once

#include "oscilline/audio/sfx.hpp"

#include <memory>

namespace oscilline {

// Low-latency playback for the mixer. This stream is not the course clock.
// CD-DA and custom music stay on CddaOutput; this sits beside that device.
// The device stays closed until a note is actually queued, so a silent run
// does not open audio.
class SfxOutput {
  public:
    static SfxOutput open(SfxMixer& mixer);

    SfxOutput(SfxOutput&&) noexcept;
    SfxOutput& operator=(SfxOutput&&) noexcept;
    ~SfxOutput();

    SfxOutput(const SfxOutput&) = delete;
    SfxOutput& operator=(const SfxOutput&) = delete;

    void set_paused(bool paused);
    // Drops samples already queued to the device. A restarted menu loop would
    // otherwise play over the tail of the loop that was stopped.
    void discard();
    void pump();

    [[nodiscard]] bool device_open() const;

  private:
    struct Impl;
    explicit SfxOutput(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace oscilline
