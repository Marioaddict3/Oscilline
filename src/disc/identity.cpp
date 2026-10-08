// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// SYSTEM.CNF BOOT line against the two accepted serials.

#include "oscilline/disc/identity.hpp"

#include <cctype>

namespace oscilline {
namespace {

std::string ascii_upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string boot_file_name(std::string_view value) {
    value = trim(value);
    const auto space = value.find_first_of(" \t");
    if (space != std::string_view::npos) {
        value = value.substr(0, space);
    }
    const auto version = value.find(';');
    if (version != std::string_view::npos) {
        value = value.substr(0, version);
    }
    const auto slash = value.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        value = value.substr(slash + 1);
    }
    return std::string(value);
}

} // namespace

const char* region_name(Region region) {
    switch (region) {
    case Region::Pal:
        return "PAL";
    case Region::Japan:
        return "JP";
    }
    return "unknown";
}

std::string unrecognized_disc_message(std::string_view boot_file) {
    std::string message = "Unrecognized disc.\n";
    if (boot_file.empty()) {
        message += "No SYSTEM.CNF boot file was found.\n";
    } else {
        message += "Boot file: '";
        message += boot_file;
        message += "'\n";
    }
    message += "Oscilline reads a disc image you own.\n";
    message += "The primary supported disc is:\n";
    message += "  PAL  SCES-02873  (boot file SCES_028.73)\n";
    message += "Japanese discs are also accepted:\n";
    message += "  JP   SCPS-45469  (boot file SCPS_454.69)\n";
    message += "These serials are the PAL and Japanese discs of Vib-Ribbon.\n";
    return message;
}

Result<DiscIdentity> identify_disc(const IsoVolume& volume) {
    const FsNode* config = volume.find("SYSTEM.CNF");
    if (config == nullptr || config->directory) {
        return Result<DiscIdentity>::failure(unrecognized_disc_message(""));
    }
    if (config->size > (1u << 20)) {
        return Result<DiscIdentity>::failure("SYSTEM.CNF is unexpectedly large");
    }
    auto bytes = volume.read_file(*config);
    if (!bytes) {
        return Result<DiscIdentity>::failure(bytes.error());
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                           bytes.value().size());
    std::string boot_value;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string_view line(text.data() + cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        cursor = end < text.size() ? end + 1 : text.size();
        line = trim(line);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string key = ascii_upper(trim(line.substr(0, equals)));
        if (key == "BOOT" && boot_value.empty()) {
            boot_value = std::string(trim(line.substr(equals + 1)));
        }
    }
    if (boot_value.empty()) {
        return Result<DiscIdentity>::failure(unrecognized_disc_message(""));
    }
    const std::string boot = boot_file_name(boot_value);
    const std::string boot_upper = ascii_upper(boot);
    DiscIdentity identity;
    identity.boot_file = boot_upper;
    if (boot_upper == "SCES_028.73") {
        identity.region = Region::Pal;
        identity.serial = "SCES-02873";
        return Result<DiscIdentity>::success(std::move(identity));
    }
    if (boot_upper == "SCPS_454.69") {
        identity.region = Region::Japan;
        identity.serial = "SCPS-45469";
        return Result<DiscIdentity>::success(std::move(identity));
    }
    return Result<DiscIdentity>::failure(unrecognized_disc_message(boot_upper));
}

} // namespace oscilline
