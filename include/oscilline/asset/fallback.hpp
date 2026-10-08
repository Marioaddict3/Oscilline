// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Silent stand-in used when a disc sound bank is absent.

#pragma once

namespace oscilline {

// Silent stand-in. A disc bank implements the same call later; until then,
// and whenever a slot stays on the placeholder, playback is this.
class SoundOutput {
  public:
    virtual ~SoundOutput() = default;
    virtual void play(int program, int note) = 0;
};

class SilentSound final : public SoundOutput {
  public:
    void play(int program, int note) override;
    int plays = 0;
};

} // namespace oscilline
