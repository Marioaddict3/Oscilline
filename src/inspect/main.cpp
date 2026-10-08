// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Command line for oscilline-inspect.

#include "oscilline/inspect.hpp"
#include "oscilline/version.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uintmax_t kMaxFile = 64u * 1024u * 1024u;

void print_usage() {
    std::cerr << "Oscilline " << oscilline::version_string() << "\n"
              << "Usage:\n"
              << "  oscilline-inspect [--type TYPE] [--full-pcm] <file>\n"
              << "  oscilline-inspect [--full-pcm] <file.vh> --vb <file.vb>\n"
              << "  oscilline-inspect [--full-pcm] --vh <file.vh> <file.vb>\n"
              << "\n"
              << "TYPE is pak, tim, tmd, anm, anc, vh, vb, or fsl.\n"
              << "The extension is used when --type is omitted.\n"
              << "A .vb file needs the matching .vh via --vh.\n";
}

std::string lower_copy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

std::string extension_type(const std::filesystem::path& path) {
    std::string ext = lower_copy(path.extension().string());
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return ext;
}

int refuse(const std::string& message) {
    std::cerr << message;
    if (message.empty() || message.back() != '\n') {
        std::cerr << '\n';
    }
    return EXIT_FAILURE;
}

oscilline::Result<std::vector<std::uint8_t>> load_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return oscilline::Result<std::vector<std::uint8_t>>::failure("cannot read '" +
                                                                     path.string() + "'");
    }
    if (size > kMaxFile) {
        return oscilline::Result<std::vector<std::uint8_t>>::failure("'" + path.string() +
                                                                     "' is unreasonably large");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return oscilline::Result<std::vector<std::uint8_t>>::failure("cannot read '" +
                                                                     path.string() + "'");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size != 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!in) {
            return oscilline::Result<std::vector<std::uint8_t>>::failure("failed while reading '" +
                                                                         path.string() + "'");
        }
    }
    return oscilline::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

} // namespace

int main(int argc, char** argv) {
    std::string type;
    std::filesystem::path file;
    std::filesystem::path vh_path;
    std::filesystem::path vb_path;
    bool full_pcm = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return EXIT_SUCCESS;
        }
        if (arg == "--version") {
            std::cout << "Oscilline " << oscilline::version_string() << '\n';
            return EXIT_SUCCESS;
        }
        if (arg == "--full-pcm") {
            full_pcm = true;
            continue;
        }
        if (arg == "--type" || arg == "--vh" || arg == "--vb") {
            if (i + 1 >= argc) {
                return refuse(std::string("oscilline-inspect: ") + std::string(arg) +
                              " needs a value");
            }
            const std::string value = argv[++i];
            if (arg == "--type") {
                type = lower_copy(value);
            } else if (arg == "--vh") {
                vh_path = value;
            } else {
                vb_path = value;
            }
            continue;
        }
        if (arg.starts_with("-")) {
            return refuse("oscilline-inspect: unknown argument '" + std::string(arg) + "'");
        }
        if (!file.empty()) {
            return refuse("oscilline-inspect: only one input file is accepted");
        }
        file = arg;
    }

    if (file.empty() && !vh_path.empty()) {
        file = vh_path;
        vh_path.clear();
    }
    if (file.empty()) {
        print_usage();
        return EXIT_FAILURE;
    }
    if (type.empty()) {
        type = extension_type(file);
    }
    if (type.empty()) {
        return refuse("oscilline-inspect: pass --type or use a known extension");
    }

    oscilline::InspectRequest request;
    request.type = type;
    request.full_pcm = full_pcm;
    if (type == "vb") {
        if (vh_path.empty()) {
            return refuse("a VB file needs the matching VH via --vh");
        }
        auto header = load_file(vh_path);
        if (!header) {
            return refuse(header.error());
        }
        auto body = load_file(file);
        if (!body) {
            return refuse(body.error());
        }
        request.bytes = std::move(header.value());
        request.body = std::move(body.value());
    } else {
        auto bytes = load_file(file);
        if (!bytes) {
            return refuse(bytes.error());
        }
        request.bytes = std::move(bytes.value());
        if (!vb_path.empty()) {
            auto body = load_file(vb_path);
            if (!body) {
                return refuse(body.error());
            }
            request.body = std::move(body.value());
        }
    }

    auto json = oscilline::inspect_to_json(request);
    if (!json) {
        return refuse(json.error());
    }
    std::cout << json.value();
    return EXIT_SUCCESS;
}
