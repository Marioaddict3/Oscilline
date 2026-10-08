// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Opens mapped files from the game, title, and audio archives.

#include "oscilline/asset/registry.hpp"

#include "oscilline/anc.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace oscilline {
namespace {

std::string canon(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch == '\\') {
            ch = '/';
        }
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return out;
}

std::string with_slash(std::string_view folder) {
    std::string prefix = canon(folder);
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    return prefix;
}

bool japanese_font(std::string_view pak_path, Region region) {
    const auto slash = pak_path.find_last_of("/\\");
    const std::string_view file =
        slash == std::string_view::npos ? pak_path : pak_path.substr(slash + 1);
    const std::string name = canon(file);
    if (name.starts_with("01_")) {
        return true;
    }
    return region == Region::Japan && name == "files.pak";
}

const PakEntry* entry_named(const PakArchive& archive, std::string_view path) {
    return find_entry(archive, path);
}

std::string join_location(std::string_view pak_path, std::string_view entry_name) {
    std::string location(pak_path);
    if (!location.empty()) {
        location.push_back(':');
    }
    location.append(entry_name);
    return location;
}

struct Pair {
    const PakEntry* tmd = nullptr;
    const PakEntry* anm = nullptr;
    int frames = 0;
    std::string stem;
};

std::vector<Pair> animation_pairs(const PakArchive& archive,
                                  std::string_view folder,
                                  std::vector<std::string>& warnings) {
    const std::string prefix = with_slash(folder);
    std::map<std::string, Pair> by_stem;
    for (const PakEntry& entry : archive.entries) {
        const std::string path = canon(entry.name);
        if (!path.starts_with(prefix)) {
            continue;
        }
        const auto dot = path.rfind('.');
        if (dot == std::string::npos || dot < prefix.size()) {
            continue;
        }
        const std::string ext = path.substr(dot);
        const std::string stem = path.substr(0, dot);
        if (ext == ".tmd") {
            by_stem[stem].tmd = &entry;
        } else if (ext == ".anm") {
            by_stem[stem].anm = &entry;
        }
    }
    std::vector<Pair> pairs;
    for (auto& [stem, pair] : by_stem) {
        if (pair.tmd == nullptr || pair.anm == nullptr) {
            continue;
        }
        auto animation = parse_anm(pair.anm->data);
        if (!animation) {
            warnings.push_back(pair.anm->name + ": " + animation.error());
            continue;
        }
        pair.frames = static_cast<int>(animation.value().frame_count);
        pair.stem = stem;
        pairs.push_back(pair);
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.frames != b.frames) {
            return a.frames > b.frames;
        }
        return a.stem < b.stem;
    });
    return pairs;
}

std::vector<std::string> folder_anims(const PakArchive& archive, std::string_view folder) {
    const std::string prefix = with_slash(folder);
    std::vector<std::string> names;
    for (const PakEntry& entry : archive.entries) {
        const std::string path = canon(entry.name);
        if (path.starts_with(prefix) && path.ends_with(".anm")) {
            names.push_back(entry.name);
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return canon(a) < canon(b);
    });
    return names;
}

const PakEntry* folder_tmd(const PakArchive& archive, std::string_view folder) {
    const std::string model_path = std::string(folder) + "/MODEL.TMD";
    if (const PakEntry* model = entry_named(archive, model_path)) {
        return model;
    }
    const std::string prefix = with_slash(folder);
    const PakEntry* found = nullptr;
    std::string best;
    for (const PakEntry& entry : archive.entries) {
        const std::string path = canon(entry.name);
        if (!path.starts_with(prefix) || !path.ends_with(".tmd")) {
            continue;
        }
        const std::string rest = path.substr(prefix.size());
        if (rest.find('/') != std::string::npos) {
            continue;
        }
        if (found == nullptr || path < best) {
            found = &entry;
            best = path;
        }
    }
    return found;
}

const PakEntry*
first_with_suffix(const PakArchive& archive, std::string_view folder, std::string_view suffix) {
    const std::string prefix = with_slash(folder);
    const std::string tail = canon(suffix);
    const PakEntry* found = nullptr;
    std::string best;
    for (const PakEntry& entry : archive.entries) {
        const std::string path = canon(entry.name);
        if (!path.starts_with(prefix) || !path.ends_with(tail)) {
            continue;
        }
        if (found == nullptr || path < best) {
            found = &entry;
            best = path;
        }
    }
    return found;
}

bool file_on_volume(const IsoVolume& volume, std::string_view path) {
    const FsNode* node = volume.find(path);
    return node != nullptr && !node->directory && !node->outside_data_track;
}

Slot menu_or_game_bank(Slot slot) {
    switch (slot) {
    case Slot::SoundMenuMove:
    case Slot::SoundConfirm:
    case Slot::SoundBack:
    case Slot::SoundAmbient:
        return Slot::SoundBankTitle;
    default:
        return Slot::SoundBankGame;
    }
}

} // namespace

AssetRegistry AssetRegistry::placeholders() {
    Parts parts;
    parts.force_placeholder = true;
    return from_parts(parts);
}

AssetRegistry AssetRegistry::from_disc(const MountedDisc& disc,
                                       bool force_placeholder,
                                       Confidence minimum,
                                       std::string_view language_pak) {
    Parts parts;
    parts.volume = &disc.volume;
    parts.region = disc.identity.region;
    parts.force_placeholder = force_placeholder;
    parts.minimum = minimum;
    if (force_placeholder) {
        return from_parts(parts);
    }

    PakArchive game;
    PakArchive title;
    std::string language_warning;
    if (!language_pak.empty()) {
        auto parsed = load_language_pak(disc.volume, language_pak);
        if (parsed) {
            game = std::move(parsed.value());
            parts.game = &game;
            parts.game_pak_path = std::string(language_pak);
        } else {
            language_warning = std::string(language_pak) + ": " + parsed.error();
        }
    }
    if (parts.game == nullptr) {
        if (const auto path = locate_language_pak(disc.volume)) {
            auto parsed = load_language_pak(disc.volume, *path);
            if (!parsed) {
                AssetRegistry failed = from_parts(parts);
                if (!language_warning.empty()) {
                    failed.warnings_.push_back(language_warning);
                }
                failed.warnings_.push_back(*path + ": " + parsed.error());
                return failed;
            }
            game = std::move(parsed.value());
            parts.game = &game;
            parts.game_pak_path = *path;
        }
    }
    const FsNode* title_node = disc.volume.find(kTitlePakPath);
    if (title_node != nullptr && !title_node->directory && !title_node->outside_data_track) {
        auto bytes = disc.volume.read_file(*title_node);
        if (!bytes) {
            parts.game = game.entries.empty() && parts.game_pak_path.empty() ? nullptr : &game;
            AssetRegistry failed = from_parts(parts);
            if (!language_warning.empty()) {
                failed.warnings_.push_back(std::move(language_warning));
            }
            failed.warnings_.push_back(std::string(kTitlePakPath) + ": " + bytes.error());
            return failed;
        }
        auto parsed = parse_pak(bytes.value());
        if (!parsed) {
            parts.game = parts.game_pak_path.empty() ? nullptr : &game;
            AssetRegistry failed = from_parts(parts);
            if (!language_warning.empty()) {
                failed.warnings_.push_back(std::move(language_warning));
            }
            failed.warnings_.push_back(std::string(kTitlePakPath) + ": " + parsed.error());
            return failed;
        }
        title = std::move(parsed.value());
        parts.title = &title;
        parts.title_pak_path = std::string(kTitlePakPath);
    }
    AssetRegistry registry = from_parts(parts);
    if (!language_warning.empty()) {
        registry.warnings_.push_back(std::move(language_warning));
    }
    return registry;
}

AssetRegistry assets_for_custom_play(const MountedDisc* disc,
                                     bool allow_disc_assets,
                                     Confidence minimum,
                                     std::string_view language_pak) {
    if (disc == nullptr || !allow_disc_assets) {
        return AssetRegistry::placeholders();
    }
    return AssetRegistry::from_disc(*disc, false, minimum, language_pak);
}

AssetRegistry AssetRegistry::from_parts(const Parts& parts) {
    AssetRegistry registry;
    registry.slots_.resize(static_cast<std::size_t>(Slot::Count));
    if (parts.game != nullptr) {
        registry.game_ = *parts.game;
        registry.have_game_ = true;
    }
    if (parts.title != nullptr) {
        registry.title_ = *parts.title;
        registry.have_title_ = true;
    }
    if (parts.volume != nullptr) {
        registry.volume_ = *parts.volume;
        registry.have_volume_ = true;
    }
    Parts owned = parts;
    owned.game = registry.have_game_ ? &registry.game_ : nullptr;
    owned.title = registry.have_title_ ? &registry.title_ : nullptr;
    owned.volume = registry.have_volume_ ? &registry.volume_ : nullptr;
    registry.minimum_ = parts.minimum;
    registry.resolve(owned);
    return registry;
}

void AssetRegistry::resolve(const Parts& parts) {
    const bool japanese = parts.game != nullptr && japanese_font(parts.game_pak_path, parts.region);
    for (const MapRow& row : asset_map()) {
        const std::size_t index = static_cast<std::size_t>(row.slot);
        if (index >= slots_.size()) {
            continue;
        }
        SlotState& state = slots_[index];
        state.kind = row.kind;
        state.program = row.program;
        state.note = row.note;
        state.bank = menu_or_game_bank(row.slot);
        if (parts.force_placeholder ||
            static_cast<int>(row.confidence) < static_cast<int>(parts.minimum) ||
            row.pick == AssetPick::None) {
            continue;
        }

        const PakArchive* archive = nullptr;
        std::string_view pak_path;
        if (row.area == AssetArea::GamePak && parts.game != nullptr) {
            archive = parts.game;
            pak_path = parts.game_pak_path;
        } else if (row.area == AssetArea::TitlePak && parts.title != nullptr) {
            archive = parts.title;
            pak_path = parts.title_pak_path;
        }

        if (row.pick == AssetPick::Exact && archive != nullptr) {
            if (const PakEntry* entry = entry_named(*archive, row.pattern)) {
                state.origin = AssetOrigin::Disc;
                state.tmd_name = entry->name;
                if (row.alt != nullptr && row.alt[0] != '\0') {
                    if (const PakEntry* animation = entry_named(*archive, row.alt)) {
                        state.anm_names.push_back(animation->name);
                    }
                }
                state.location = join_location(pak_path, entry->name);
            }
        } else if (row.pick == AssetPick::LanguageFont && archive != nullptr) {
            const char* preferred = japanese ? row.alt : row.pattern;
            const char* fallback = japanese ? row.pattern : row.alt;
            const PakEntry* entry = entry_named(*archive, preferred);
            if (entry == nullptr && fallback != nullptr && fallback[0] != '\0') {
                entry = entry_named(*archive, fallback);
            }
            if (entry != nullptr) {
                state.origin = AssetOrigin::Disc;
                state.tmd_name = entry->name;
                state.location = join_location(pak_path, entry->name);
            }
        } else if (row.pick == AssetPick::FolderModel && archive != nullptr) {
            const std::string model_path = std::string(row.pattern) + "/MODEL.TMD";
            if (const PakEntry* entry = entry_named(*archive, model_path)) {
                state.origin = AssetOrigin::Disc;
                state.tmd_name = entry->name;
                state.anm_names = folder_anims(*archive, row.pattern);
                state.location = join_location(pak_path, entry->name);
            }
        } else if (row.pick == AssetPick::FolderPair && archive != nullptr) {
            const std::vector<Pair> pairs = animation_pairs(*archive, row.pattern, warnings_);
            if (row.pick_index >= 0 && static_cast<std::size_t>(row.pick_index) < pairs.size()) {
                const Pair& pair = pairs[static_cast<std::size_t>(row.pick_index)];
                state.origin = AssetOrigin::Disc;
                state.tmd_name = pair.tmd->name;
                state.anm_names.push_back(pair.anm->name);
                state.location = join_location(pak_path, pair.tmd->name);
            }
        } else if (row.pick == AssetPick::FolderSuffix && archive != nullptr) {
            if (const PakEntry* entry = first_with_suffix(*archive, row.pattern, row.alt)) {
                state.origin = AssetOrigin::Disc;
                state.tmd_name = entry->name;
                state.location = join_location(pak_path, entry->name);
            }
        } else if (row.pick == AssetPick::FolderTmd && archive != nullptr) {
            if (const PakEntry* entry = folder_tmd(*archive, row.pattern)) {
                state.origin = AssetOrigin::Disc;
                state.tmd_name = entry->name;
                state.anm_names = folder_anims(*archive, row.pattern);
                state.location = join_location(pak_path, entry->name);
            }
        } else if (row.pick == AssetPick::AudioStem && parts.volume != nullptr) {
            const std::string vh = std::string(row.pattern) + ".VH";
            const std::string vb = std::string(row.pattern) + ".VB";
            if (file_on_volume(*parts.volume, vh) && file_on_volume(*parts.volume, vb)) {
                state.origin = AssetOrigin::Disc;
                state.audio_stem = row.pattern;
                state.location = vh;
            }
        }

        if (state.origin == AssetOrigin::Disc && state.kind != AssetKind::Sound) {
            preload_.push_back(row.slot);
        }
    }
}

AssetOrigin AssetRegistry::origin(Slot slot) const {
    return state_at(slot).origin;
}

const std::string& AssetRegistry::location(Slot slot) const {
    return state_at(slot).location;
}

const std::vector<std::string>& AssetRegistry::warnings() const {
    return warnings_;
}

std::string AssetRegistry::report() const {
    std::string out;
    for (const MapRow& row : asset_map()) {
        const SlotState& state = state_at(row.slot);
        std::string line = row.name;
        if (line.size() < 22) {
            line.append(22 - line.size(), ' ');
        }
        if (state.origin == AssetOrigin::Disc) {
            line += "disc";
            if (!state.location.empty()) {
                line += "  ";
                line += state.location;
            }
        } else {
            line += "placeholder";
        }
        line.push_back('\n');
        out += line;
    }
    return out;
}

void AssetRegistry::demote(SlotState& state, std::string warning) {
    state.origin = AssetOrigin::Placeholder;
    state.model.reset();
    state.anims.clear();
    state.loaded_anm_names.clear();
    state.camera.reset();
    state.bank_data.reset();
    state.location.clear();
    state.loaded = true;
    warnings_.push_back(std::move(warning));
}

void AssetRegistry::load(Slot slot) {
    SlotState& state = state_at(slot);
    if (state.loaded || state.origin != AssetOrigin::Disc) {
        state.loaded = true;
        return;
    }
    state.loaded = true;
    const PakArchive* archive = nullptr;
    if (have_game_ && find_entry(game_, state.tmd_name) != nullptr) {
        archive = &game_;
    } else if (have_title_ && find_entry(title_, state.tmd_name) != nullptr) {
        archive = &title_;
    }

    if (state.kind == AssetKind::SoundBank) {
        if (!have_volume_ || state.audio_stem.empty()) {
            demote(state, "sound bank is missing its VH/VB pair");
            return;
        }
        const FsNode* vh_node = volume_.find(state.audio_stem + ".VH");
        const FsNode* vb_node = volume_.find(state.audio_stem + ".VB");
        if (vh_node == nullptr || vb_node == nullptr) {
            demote(state, state.audio_stem + ": sound bank files are missing");
            return;
        }
        auto vh = volume_.read_file(*vh_node);
        auto vb = volume_.read_file(*vb_node);
        if (!vh || !vb) {
            demote(state, state.audio_stem + ": " + (vh ? vb.error() : vh.error()));
            return;
        }
        auto bank = parse_vab(vh.value(), vb.value());
        if (!bank) {
            demote(state, state.audio_stem + ": " + bank.error());
            return;
        }
        state.bank_data = std::move(bank.value());
        return;
    }

    if (state.kind == AssetKind::Sound) {
        return;
    }
    if (state.kind == AssetKind::CameraPath) {
        if (archive == nullptr) {
            demote(state, state.tmd_name + ": archive entry disappeared");
            return;
        }
        const PakEntry* entry = find_entry(*archive, state.tmd_name);
        if (entry == nullptr) {
            demote(state, state.tmd_name + ": camera entry is missing");
            return;
        }
        auto parsed = parse_anc(entry->data);
        if (!parsed) {
            demote(state, state.tmd_name + ": " + parsed.error());
            return;
        }
        state.camera = std::move(parsed.value());
        return;
    }
    if (archive == nullptr) {
        demote(state, state.tmd_name + ": archive entry disappeared");
        return;
    }
    const PakEntry* tmd_entry = find_entry(*archive, state.tmd_name);
    if (tmd_entry == nullptr) {
        demote(state, state.tmd_name + ": model entry is missing");
        return;
    }
    auto model = parse_tmd(tmd_entry->data);
    if (!model) {
        demote(state, state.tmd_name + ": " + model.error());
        return;
    }
    state.model = std::move(model.value());
    for (const std::string& name : state.anm_names) {
        const PakEntry* anm_entry = find_entry(*archive, name);
        if (anm_entry == nullptr) {
            warnings_.push_back(name + ": animation entry is missing");
            continue;
        }
        auto animation = parse_anm(anm_entry->data);
        if (!animation) {
            warnings_.push_back(name + ": " + animation.error());
            continue;
        }
        state.loaded_anm_names.push_back(name);
        state.anims.push_back(std::move(animation.value()));
    }
}

int AssetRegistry::preload_steps() const {
    return static_cast<int>(preload_.size());
}

void AssetRegistry::preload_step(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= preload_.size()) {
        return;
    }
    load(preload_[static_cast<std::size_t>(index)]);
}

void AssetRegistry::preload_all() {
    const int steps = preload_steps();
    for (int step = 0; step < steps; ++step) {
        preload_step(step);
    }
}

const TmdModel* AssetRegistry::model(Slot slot) {
    load(slot);
    const SlotState& state = state_at(slot);
    if (state.origin != AssetOrigin::Disc || !state.model) {
        return nullptr;
    }
    return &state.model.value();
}

const std::vector<AnmFile>* AssetRegistry::animations(Slot slot) {
    load(slot);
    const SlotState& state = state_at(slot);
    if (state.origin != AssetOrigin::Disc) {
        return nullptr;
    }
    return &state.anims;
}

const std::vector<std::string>* AssetRegistry::animation_names(Slot slot) {
    load(slot);
    const SlotState& state = state_at(slot);
    if (state.origin != AssetOrigin::Disc) {
        return nullptr;
    }
    return &state.loaded_anm_names;
}

const VabBank* AssetRegistry::sound_bank(Slot slot) {
    load(slot);
    const SlotState& state = state_at(slot);
    if (state.origin != AssetOrigin::Disc || !state.bank_data) {
        return nullptr;
    }
    return &state.bank_data.value();
}

const AncFile* AssetRegistry::camera(Slot slot) {
    load(slot);
    const SlotState& state = state_at(slot);
    if (state.origin != AssetOrigin::Disc || !state.camera) {
        return nullptr;
    }
    return &state.camera.value();
}

SoundCue AssetRegistry::sound(Slot slot) const {
    const SlotState& state = state_at(slot);
    SoundCue cue;
    cue.origin = state.origin;
    cue.bank = state.bank;
    cue.program = state.program;
    cue.note = state.note;
    if (cue.program < 0 || cue.note < 0) {
        cue.origin = AssetOrigin::Placeholder;
    }
    return cue;
}

AssetRegistry::SlotState& AssetRegistry::state_at(Slot slot) {
    return slots_[static_cast<std::size_t>(slot)];
}

const AssetRegistry::SlotState& AssetRegistry::state_at(Slot slot) const {
    return slots_[static_cast<std::size_t>(slot)];
}

} // namespace oscilline
