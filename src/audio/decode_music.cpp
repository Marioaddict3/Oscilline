// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Decodes WAV, FLAC, and MP3 to 44100 Hz stereo with dr_libs.

#include "oscilline/audio/cdda.hpp"
#include "oscilline/course/music.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

namespace oscilline {
namespace {

std::string lower_ext(std::string_view name) {
    std::filesystem::path path{std::string(name)};
    std::string ext = path.extension().string();
    for (char& letter : ext) {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return ext;
}

void mix_down(const std::int16_t* in, int channels, int frames, std::vector<std::int16_t>& stereo) {
    const std::size_t base = stereo.size();
    stereo.resize(base + static_cast<std::size_t>(frames) * 2u);
    for (int i = 0; i < frames; ++i) {
        std::int16_t left = 0;
        std::int16_t right = 0;
        if (channels <= 1) {
            left = right = in[i];
        } else if (channels == 2) {
            left = in[static_cast<std::size_t>(i) * 2u];
            right = in[static_cast<std::size_t>(i) * 2u + 1u];
        } else {
            std::int32_t sum = 0;
            for (int channel = 0; channel < channels; ++channel) {
                sum += in[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) +
                          static_cast<std::size_t>(channel)];
            }
            const std::int32_t mixed = sum / channels;
            left = right = static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
        }
        stereo[base + static_cast<std::size_t>(i) * 2u] = left;
        stereo[base + static_cast<std::size_t>(i) * 2u + 1u] = right;
    }
}

void fingerprint_of(const std::vector<std::int16_t>& samples,
                    std::string& hex,
                    std::uint32_t& seed) {
    std::uint64_t h1 = 14695981039346656037ull;
    std::uint64_t h2 = 0x6a09e667f3bcc909ull;
    for (const std::int16_t sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        const unsigned char bytes[2] = {static_cast<unsigned char>(value & 0xffu),
                                        static_cast<unsigned char>((value >> 8) & 0xffu)};
        for (const unsigned char byte : bytes) {
            h1 ^= byte;
            h1 *= 1099511628211ull;
            h2 ^= byte;
            h2 *= 0xbf58476d1ce4e5b9ull;
        }
    }
    seed = static_cast<std::uint32_t>(h1);
    std::ostringstream out;
    out << std::hex;
    const auto put = [&](std::uint64_t value) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            out << "0123456789abcdef"[(value >> shift) & 0xfull];
        }
    };
    put(h1);
    put(h2);
    hex = out.str();
}

std::vector<std::int16_t> resample_stereo(const std::vector<std::int16_t>& source,
                                          int source_frames,
                                          int source_rate,
                                          int max_frames,
                                          bool& truncated) {
    std::vector<std::int16_t> out;
    if (source_frames <= 0 || source_rate <= 0 || max_frames <= 0) {
        return out;
    }
    if (source_rate == kCddaRate) {
        int frames = source_frames;
        if (frames > max_frames) {
            frames = max_frames;
            truncated = true;
        }
        out.assign(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(frames) * 2);
        return out;
    }
    const double step = static_cast<double>(source_rate) / static_cast<double>(kCddaRate);
    int frames = static_cast<int>(static_cast<double>(source_frames - 1) / step);
    if (frames < 1) {
        frames = 1;
    }
    if (frames > max_frames) {
        frames = max_frames;
        truncated = true;
    }
    out.resize(static_cast<std::size_t>(frames) * 2u);
    for (int i = 0; i < frames; ++i) {
        const double position = static_cast<double>(i) * step;
        const int i0 = std::min(static_cast<int>(position), source_frames - 1);
        const int i1 = std::min(i0 + 1, source_frames - 1);
        const double fraction = position - static_cast<double>(i0);
        for (int channel = 0; channel < 2; ++channel) {
            const double a =
                source[static_cast<std::size_t>(i0) * 2u + static_cast<std::size_t>(channel)];
            const double b =
                source[static_cast<std::size_t>(i1) * 2u + static_cast<std::size_t>(channel)];
            const int mixed = static_cast<int>(std::lround(a + (b - a) * fraction));
            out[static_cast<std::size_t>(i) * 2u + static_cast<std::size_t>(channel)] =
                static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
        }
    }
    return out;
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool tag_is(const std::uint8_t* bytes, const char* text) {
    return bytes[0] == static_cast<std::uint8_t>(text[0]) &&
           bytes[1] == static_cast<std::uint8_t>(text[1]) &&
           bytes[2] == static_cast<std::uint8_t>(text[2]) &&
           bytes[3] == static_cast<std::uint8_t>(text[3]);
}

// A RIFF/RF64 WAVE chunk that claims more payload than the container holds.
// dr_wav clamps that claim, which would otherwise look like a short track.
bool wave_bytes_truncated(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !(tag_is(bytes.data(), "RIFF") || tag_is(bytes.data(), "RF64")) ||
        !tag_is(bytes.data() + 8, "WAVE")) {
        return false;
    }
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::uint32_t size = read_le32(bytes.data() + pos + 4);
        if (size == 0xffffffffu) {
            return false;
        }
        if (static_cast<std::uint64_t>(pos) + 8u + static_cast<std::uint64_t>(size) >
            bytes.size()) {
            return true;
        }
        const std::uint64_t advance = 8ull + static_cast<std::uint64_t>(size) + (size & 1u);
        if (pos + advance > bytes.size()) {
            break;
        }
        pos += static_cast<std::size_t>(advance);
    }
    return false;
}

bool wave_file_truncated(const std::filesystem::path& file) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(file, error);
    if (error || file_size < 12) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::uint8_t header[12];
    in.read(reinterpret_cast<char*>(header), 12);
    if (!in || !(tag_is(header, "RIFF") || tag_is(header, "RF64")) || !tag_is(header + 8, "WAVE")) {
        return false;
    }
    std::uint64_t pos = 12;
    while (pos + 8 <= file_size) {
        std::uint8_t chunk[8];
        in.read(reinterpret_cast<char*>(chunk), 8);
        if (!in) {
            return true;
        }
        const std::uint32_t size = read_le32(chunk + 4);
        if (size == 0xffffffffu) {
            return false;
        }
        if (pos + 8u + static_cast<std::uint64_t>(size) > file_size) {
            return true;
        }
        const std::uint64_t advance = 8ull + static_cast<std::uint64_t>(size) + (size & 1u);
        const std::uint64_t skip = advance - 8u;
        in.seekg(static_cast<std::streamoff>(skip), std::ios::cur);
        if (!in) {
            return true;
        }
        pos += advance;
    }
    return false;
}

using ReadFrames = std::uint64_t (*)(void* decoder, std::uint64_t frames, std::int16_t* dest);

std::uint64_t read_wav(void* decoder, std::uint64_t frames, std::int16_t* dest) {
    return drwav_read_pcm_frames_s16(static_cast<drwav*>(decoder), frames, dest);
}

std::uint64_t read_flac(void* decoder, std::uint64_t frames, std::int16_t* dest) {
    return drflac_read_pcm_frames_s16(static_cast<drflac*>(decoder), frames, dest);
}

std::uint64_t read_mp3(void* decoder, std::uint64_t frames, std::int16_t* dest) {
    return drmp3_read_pcm_frames_s16(static_cast<drmp3*>(decoder), frames, dest);
}

Result<DecodedMusic> finish_decode(void* decoder,
                                   ReadFrames read,
                                   int channels,
                                   int rate,
                                   std::uint64_t total_frames,
                                   MusicLimits limits,
                                   MusicProgress progress,
                                   void* progress_user) {
    if (channels < 1 || channels > 8 || rate < 8000 || rate > 192000) {
        return Result<DecodedMusic>::failure("could not read that music file");
    }
    if (limits.max_seconds < 1) {
        limits.max_seconds = 1;
    }
    if (limits.min_ms < 0) {
        limits.min_ms = 0;
    }
    const std::int64_t max_out =
        static_cast<std::int64_t>(limits.max_seconds) * static_cast<std::int64_t>(kCddaRate);
    const std::int64_t max_source =
        max_out * static_cast<std::int64_t>(rate) / static_cast<std::int64_t>(kCddaRate) + 8;
    const bool length_known = total_frames > 0 && total_frames < (std::uint64_t{1} << 62);
    const std::int64_t expected =
        length_known ? static_cast<std::int64_t>(total_frames) : max_source;
    bool truncated = false;
    bool hit_cap = false;
    std::vector<std::int16_t> stereo;
    std::vector<std::int16_t> chunk(static_cast<std::size_t>(channels) * 4096u);
    std::int64_t got = 0;
    while (got < max_source) {
        const std::uint64_t ask =
            std::min<std::uint64_t>(4096u, static_cast<std::uint64_t>(max_source - got));
        const std::uint64_t n = read(decoder, ask, chunk.data());
        if (n == 0) {
            break;
        }
        mix_down(chunk.data(), channels, static_cast<int>(n), stereo);
        got += static_cast<std::int64_t>(n);
        if (progress != nullptr) {
            const float fraction =
                expected > 0 ? std::min(1.f, static_cast<float>(got) / static_cast<float>(expected))
                             : 1.f;
            if (!progress(progress_user, fraction, "READING")) {
                return Result<DecodedMusic>::failure("closed");
            }
        }
        if (n < ask) {
            break;
        }
    }
    if (got >= max_source) {
        hit_cap = true;
        std::vector<std::int16_t> extra(static_cast<std::size_t>(channels));
        if (read(decoder, 1, extra.data()) > 0) {
            truncated = true;
        }
    }
    if (got == 0) {
        return Result<DecodedMusic>::failure("could not read that music file");
    }
    // A short read against a known length is a damaged file. Stopping at the
    // 15 minute cap is the intentional cut, not damage.
    const bool short_read = length_known && got + static_cast<std::int64_t>(rate) / 5 <
                                                static_cast<std::int64_t>(total_frames);
    if (short_read && !hit_cap) {
        return Result<DecodedMusic>::failure("that file is damaged");
    }
    if (short_read && hit_cap) {
        truncated = true;
    }
    DecodedMusic decoded;
    decoded.interleaved =
        resample_stereo(stereo, static_cast<int>(got), rate, static_cast<int>(max_out), truncated);
    decoded.frames = static_cast<int>(decoded.interleaved.size() / 2u);
    decoded.truncated = truncated;
    const std::int64_t duration_ms =
        static_cast<std::int64_t>(decoded.frames) * 1000 / static_cast<std::int64_t>(kCddaRate);
    if (duration_ms < limits.min_ms) {
        return Result<DecodedMusic>::failure("that track is under " +
                                             std::to_string(limits.min_ms / 1000) + " seconds");
    }
    fingerprint_of(decoded.interleaved, decoded.fingerprint, decoded.seed);
    return Result<DecodedMusic>::success(std::move(decoded));
}

} // namespace

Result<DecodedMusic> decode_music_bytes(std::span<const std::uint8_t> bytes,
                                        std::string_view name,
                                        MusicLimits limits,
                                        MusicProgress progress,
                                        void* progress_user) {
    if (bytes.empty()) {
        return Result<DecodedMusic>::failure("could not read that music file");
    }
    const std::string ext = lower_ext(name);
    if (ext == ".wav") {
        if (wave_bytes_truncated(bytes)) {
            return Result<DecodedMusic>::failure("that file is damaged");
        }
        drwav wav;
        if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr)) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        const auto decoded = finish_decode(&wav,
                                           read_wav,
                                           static_cast<int>(wav.channels),
                                           static_cast<int>(wav.sampleRate),
                                           wav.totalPCMFrameCount,
                                           limits,
                                           progress,
                                           progress_user);
        drwav_uninit(&wav);
        return decoded;
    }
    if (ext == ".flac") {
        drflac* flac = drflac_open_memory(bytes.data(), bytes.size(), nullptr);
        if (flac == nullptr) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        const auto decoded = finish_decode(flac,
                                           read_flac,
                                           static_cast<int>(flac->channels),
                                           static_cast<int>(flac->sampleRate),
                                           flac->totalPCMFrameCount,
                                           limits,
                                           progress,
                                           progress_user);
        drflac_close(flac);
        return decoded;
    }
    if (ext == ".mp3") {
        drmp3 mp3;
        if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr)) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        std::uint64_t total = drmp3_get_pcm_frame_count(&mp3);
        if (total == DRMP3_UINT64_MAX) {
            total = 0;
        }
        const auto decoded = finish_decode(&mp3,
                                           read_mp3,
                                           static_cast<int>(mp3.channels),
                                           static_cast<int>(mp3.sampleRate),
                                           total,
                                           limits,
                                           progress,
                                           progress_user);
        drmp3_uninit(&mp3);
        return decoded;
    }
    return Result<DecodedMusic>::failure("that file is not wav, flac, or mp3");
}

Result<DecodedMusic> decode_music_file(const std::filesystem::path& file,
                                       MusicLimits limits,
                                       MusicProgress progress,
                                       void* progress_user) {
    std::error_code missing;
    if (!std::filesystem::exists(file, missing) || missing) {
        return Result<DecodedMusic>::failure("that file is missing");
    }
    const std::string path = file.string();
    const std::string ext = lower_ext(path);
    if (ext == ".wav") {
        if (wave_file_truncated(file)) {
            return Result<DecodedMusic>::failure("that file is damaged");
        }
        drwav wav;
        if (!drwav_init_file(&wav, path.c_str(), nullptr)) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        const auto decoded = finish_decode(&wav,
                                           read_wav,
                                           static_cast<int>(wav.channels),
                                           static_cast<int>(wav.sampleRate),
                                           wav.totalPCMFrameCount,
                                           limits,
                                           progress,
                                           progress_user);
        drwav_uninit(&wav);
        return decoded;
    }
    if (ext == ".flac") {
        drflac* flac = drflac_open_file(path.c_str(), nullptr);
        if (flac == nullptr) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        const auto decoded = finish_decode(flac,
                                           read_flac,
                                           static_cast<int>(flac->channels),
                                           static_cast<int>(flac->sampleRate),
                                           flac->totalPCMFrameCount,
                                           limits,
                                           progress,
                                           progress_user);
        drflac_close(flac);
        return decoded;
    }
    if (ext == ".mp3") {
        drmp3 mp3;
        if (!drmp3_init_file(&mp3, path.c_str(), nullptr)) {
            return Result<DecodedMusic>::failure("could not read that music file");
        }
        std::uint64_t total = drmp3_get_pcm_frame_count(&mp3);
        if (total == DRMP3_UINT64_MAX) {
            total = 0;
        }
        const auto decoded = finish_decode(&mp3,
                                           read_mp3,
                                           static_cast<int>(mp3.channels),
                                           static_cast<int>(mp3.sampleRate),
                                           total,
                                           limits,
                                           progress,
                                           progress_user);
        drmp3_uninit(&mp3);
        return decoded;
    }
    if (ext.empty()) {
        return Result<DecodedMusic>::failure("that file is not wav, flac, or mp3");
    }
    return Result<DecodedMusic>::failure("that file is not wav, flac, or mp3");
}

} // namespace oscilline
