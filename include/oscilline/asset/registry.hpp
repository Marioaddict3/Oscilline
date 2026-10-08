// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Resolves mapped slots from a mounted disc at runtime.

#pragma once

#include "oscilline/anc.hpp"
#include "oscilline/anm.hpp"
#include "oscilline/asset/map.hpp"
#include "oscilline/disc/archive.hpp"
#include "oscilline/tmd.hpp"
#include "oscilline/vab.hpp"

#include <optional>
#include <string>
#include <vector>

namespace oscilline {

enum class AssetOrigin : std::uint8_t { Placeholder, Disc };

struct SoundCue {
    AssetOrigin origin = AssetOrigin::Placeholder;
    Slot bank = Slot::SoundBankGame;
    int program = -1;
    int note = -1;
};

// Resolves the asset map against a mounted disc, or against nothing.
// Parsing is lazy and cached for the life of this object. SDL is not used.
// Boot-logo images are not a slot and are never read.
class AssetRegistry {
  public:
    struct Parts {
        const PakArchive* game = nullptr;
        std::string game_pak_path;
        const PakArchive* title = nullptr;
        std::string title_pak_path;
        const IsoVolume* volume = nullptr;
        Region region = Region::Pal;
        bool force_placeholder = false;
        Confidence minimum = kUsableConfidence;
    };

    [[nodiscard]] static AssetRegistry placeholders();
    [[nodiscard]] static AssetRegistry from_parts(const Parts& parts);
    [[nodiscard]] static AssetRegistry from_disc(const MountedDisc& disc,
                                                 bool force_placeholder,
                                                 Confidence minimum = kUsableConfidence,
                                                 std::string_view language_pak = {});

    [[nodiscard]] AssetOrigin origin(Slot slot) const;
    [[nodiscard]] const std::string& location(Slot slot) const;
    [[nodiscard]] const std::vector<std::string>& warnings() const;

    // Slot name, then "disc" plus a path or "placeholder". Names only.
    [[nodiscard]] std::string report() const;

    // Parses a disc slot. A malformed file becomes a placeholder and a warning.
    void load(Slot slot);

    [[nodiscard]] int preload_steps() const;
    void preload_step(int index);
    void preload_all();

    [[nodiscard]] Confidence minimum() const { return minimum_; }

    [[nodiscard]] const TmdModel* model(Slot slot);
    [[nodiscard]] const std::vector<AnmFile>* animations(Slot slot);
    // Same order as animations(). Null when the slot is not a disc model.
    [[nodiscard]] const std::vector<std::string>* animation_names(Slot slot);
    [[nodiscard]] const VabBank* sound_bank(Slot slot);
    [[nodiscard]] SoundCue sound(Slot slot) const;
    // Parsed camera path. Null when the slot is a placeholder or the file failed.
    [[nodiscard]] const AncFile* camera(Slot slot);

  private:
    struct SlotState {
        AssetOrigin origin = AssetOrigin::Placeholder;
        AssetKind kind = AssetKind::Model;
        std::string location;
        std::string tmd_name;
        std::vector<std::string> anm_names;
        std::string audio_stem;
        int program = -1;
        int note = -1;
        Slot bank = Slot::SoundBankGame;
        bool loaded = false;
        std::optional<TmdModel> model;
        std::vector<AnmFile> anims;
        std::vector<std::string> loaded_anm_names;
        std::optional<AncFile> camera;
        std::optional<VabBank> bank_data;
    };

    AssetRegistry() = default;

    void resolve(const Parts& parts);
    void demote(SlotState& state, std::string warning);
    [[nodiscard]] SlotState& state_at(Slot slot);
    [[nodiscard]] const SlotState& state_at(Slot slot) const;

    std::vector<SlotState> slots_;
    PakArchive game_;
    PakArchive title_;
    bool have_game_ = false;
    bool have_title_ = false;
    IsoVolume volume_;
    bool have_volume_ = false;
    std::vector<std::string> warnings_;
    std::vector<Slot> preload_;
    Confidence minimum_ = kUsableConfidence;
};

// Custom play uses the same disc map as a built-in course when `disc` is
// mounted and assets are allowed. Otherwise every slot stays a placeholder.
[[nodiscard]] AssetRegistry assets_for_custom_play(const MountedDisc* disc,
                                                   bool allow_disc_assets,
                                                   Confidence minimum = kUsableConfidence,
                                                   std::string_view language_pak = {});

} // namespace oscilline
