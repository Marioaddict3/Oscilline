// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// CUE tokens, track indexes, and BIN paths.

#include "oscilline/disc/cue.hpp"

#include <cctype>
#include <charconv>

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

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[i])) != 0) {
            ++i;
            continue;
        }
        if (line[i] == '"') {
            ++i;
            std::string quoted;
            while (i < line.size() && line[i] != '"') {
                quoted.push_back(line[i]);
                ++i;
            }
            if (i >= line.size() || line[i] != '"') {
                tokens.clear();
                tokens.emplace_back("\x01"); // sentinel: unmatched quote
                return tokens;
            }
            ++i;
            tokens.push_back(std::move(quoted));
            continue;
        }
        const std::size_t start = i;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0) {
            ++i;
        }
        tokens.emplace_back(line.substr(start, i - start));
    }
    return tokens;
}

Result<int> parse_int(std::string_view text) {
    int value = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return Result<int>::failure("expected an integer, got '" + std::string(text) + "'");
    }
    return Result<int>::success(value);
}

Result<CueIndex> parse_msf(int number, std::string_view text) {
    const auto first = text.find(':');
    const auto second = text.find(':', first == std::string_view::npos ? 0 : first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
        return Result<CueIndex>::failure("INDEX time must be MM:SS:FF, got '" + std::string(text) +
                                         "'");
    }
    const auto minutes = parse_int(text.substr(0, first));
    const auto seconds = parse_int(text.substr(first + 1, second - first - 1));
    const auto frames = parse_int(text.substr(second + 1));
    if (!minutes || !seconds || !frames) {
        return Result<CueIndex>::failure("INDEX time must be MM:SS:FF, got '" + std::string(text) +
                                         "'");
    }
    if (minutes.value() < 0 || seconds.value() < 0 || seconds.value() > 59 || frames.value() < 0 ||
        frames.value() > 74) {
        return Result<CueIndex>::failure("INDEX time is out of range: '" + std::string(text) + "'");
    }
    CueIndex index;
    index.number = number;
    index.minutes = minutes.value();
    index.seconds = seconds.value();
    index.frames = frames.value();
    return Result<CueIndex>::success(index);
}

Result<RawSectorKind> parse_track_mode(std::string_view mode) {
    const std::string upper = ascii_upper(mode);
    if (upper == "AUDIO") {
        return Result<RawSectorKind>::success(RawSectorKind::Audio2352);
    }
    if (upper == "MODE1/2048") {
        return Result<RawSectorKind>::success(RawSectorKind::Mode1_2048);
    }
    if (upper == "MODE1/2352") {
        return Result<RawSectorKind>::success(RawSectorKind::Mode1_2352);
    }
    if (upper == "MODE2/2336") {
        return Result<RawSectorKind>::success(RawSectorKind::Mode2_2336);
    }
    if (upper == "MODE2/2352") {
        return Result<RawSectorKind>::success(RawSectorKind::Mode2_2352);
    }
    return Result<RawSectorKind>::failure("unsupported cue track mode '" + std::string(mode) + "'");
}

} // namespace

Result<CueSheet> parse_cue(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    CueSheet sheet;
    std::string current_file;
    std::string current_type;
    int line_number = 0;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        std::size_t end = text.find('\n', cursor);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        cursor = end + 1;
        ++line_number;
        if (line.empty()) {
            if (end == text.size()) {
                break;
            }
            continue;
        }

        const auto tokens = tokenize(line);
        if (!tokens.empty() && tokens[0] == "\x01") {
            return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                             " has an unmatched quote");
        }
        if (tokens.empty()) {
            if (end == text.size()) {
                break;
            }
            continue;
        }
        const std::string command = ascii_upper(tokens[0]);
        if (command == "REM" || command == "CATALOG" || command == "PERFORMER" ||
            command == "TITLE" || command == "SONGWRITER" || command == "FLAGS" ||
            command == "CDTEXTFILE" || command == "ISRC" || command == "POSTGAP") {
            if (end == text.size()) {
                break;
            }
            continue;
        }
        if (command == "FILE") {
            if (tokens.size() < 3) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " FILE needs a name and a type");
            }
            current_file = tokens[1];
            current_type = ascii_upper(tokens[2]);
            if (current_type != "BINARY") {
                return Result<CueSheet>::failure(
                    "cue file '" + current_file +
                    "' is not BINARY; Oscilline reads raw BIN audio and data tracks");
            }
        } else if (command == "TRACK") {
            if (current_file.empty()) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " TRACK appears before FILE");
            }
            if (tokens.size() < 3) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " TRACK needs a number and a mode");
            }
            const auto number = parse_int(tokens[1]);
            if (!number || number.value() <= 0) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " has a bad track number");
            }
            const auto kind = parse_track_mode(tokens[2]);
            if (!kind) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) + ": " +
                                                 kind.error());
            }
            CueTrack track;
            track.number = number.value();
            track.kind = kind.value();
            track.file_name = current_file;
            track.file_type = current_type;
            sheet.tracks.push_back(std::move(track));
        } else if (command == "INDEX") {
            if (sheet.tracks.empty()) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " INDEX appears before TRACK");
            }
            if (tokens.size() < 3) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " INDEX needs a number and MM:SS:FF");
            }
            const auto number = parse_int(tokens[1]);
            if (!number) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) + ": " +
                                                 number.error());
            }
            const auto index = parse_msf(number.value(), tokens[2]);
            if (!index) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) + ": " +
                                                 index.error());
            }
            sheet.tracks.back().indices.push_back(index.value());
        } else if (command == "PREGAP") {
            if (sheet.tracks.empty() || tokens.size() < 2) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                                 " PREGAP is incomplete");
            }
            const auto gap = parse_msf(0, tokens[1]);
            if (!gap) {
                return Result<CueSheet>::failure("cue line " + std::to_string(line_number) + ": " +
                                                 gap.error());
            }
            sheet.tracks.back().has_pregap = true;
            sheet.tracks.back().pregap_sectors = gap.value().as_sectors();
        } else {
            return Result<CueSheet>::failure("cue line " + std::to_string(line_number) +
                                             " has unknown command '" + tokens[0] + "'");
        }
        if (end == text.size()) {
            break;
        }
    }

    if (sheet.tracks.empty()) {
        return Result<CueSheet>::failure("cue sheet has no tracks");
    }
    for (const CueTrack& track : sheet.tracks) {
        bool saw_index1 = false;
        for (const CueIndex& index : track.indices) {
            if (index.number == 1) {
                saw_index1 = true;
            }
        }
        if (!saw_index1) {
            return Result<CueSheet>::failure("cue track " + std::to_string(track.number) +
                                             " has no INDEX 01");
        }
    }
    return Result<CueSheet>::success(std::move(sheet));
}

} // namespace oscilline
