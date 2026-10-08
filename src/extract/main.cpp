// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Oscilline contributors
//
// Lists or copies files from a disc image the user owns.

#include "oscilline/disc/identity.hpp"
#include "oscilline/disc/image.hpp"
#include "oscilline/disc/iso9660.hpp"
#include "oscilline/version.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_usage() {
    std::cerr << "Oscilline " << oscilline::version_string() << "\n"
              << "Usage:\n"
              << "  oscilline-extract list <image>\n"
              << "  oscilline-extract extract <image> <outdir> [path]\n"
              << "\n"
              << "image is an .iso, a .cue, or a .bin with a sibling .cue.\n"
              << "Files are written only under <outdir>. Unknown discs are refused.\n";
}

bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const std::filesystem::path normal_root = root.lexically_normal();
    const std::filesystem::path normal = candidate.lexically_normal();
    auto root_it = normal_root.begin();
    auto cand_it = normal.begin();
    for (; root_it != normal_root.end(); ++root_it, ++cand_it) {
        if (cand_it == normal.end() || *root_it != *cand_it) {
            return false;
        }
    }
    return true;
}

std::filesystem::path relative_destination(const std::filesystem::path& outdir,
                                           std::string iso_path) {
    while (!iso_path.empty() && (iso_path.front() == '/' || iso_path.front() == '\\')) {
        iso_path.erase(iso_path.begin());
    }
    const std::filesystem::path relative{iso_path};
    for (const std::filesystem::path& part : relative) {
        if (part == "..") {
            return {};
        }
    }
    const std::filesystem::path destination = outdir / relative;
    if (!path_is_inside(outdir, destination)) {
        return {};
    }
    return destination;
}

int refuse(const std::string& message) {
    std::cerr << message;
    if (message.empty() || message.back() != '\n') {
        std::cerr << '\n';
    }
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return EXIT_FAILURE;
    }
    const std::string_view command{argv[1]};
    const std::filesystem::path image_path{argv[2]};

    auto image = oscilline::DiscImage::open(image_path);
    if (!image) {
        return refuse(image.error());
    }
    auto volume = oscilline::IsoVolume::read(image.value());
    if (!volume) {
        return refuse(volume.error());
    }
    auto identity = oscilline::identify_disc(volume.value());
    if (!identity) {
        return refuse(identity.error());
    }

    if (command == "list") {
        if (argc != 3) {
            print_usage();
            return EXIT_FAILURE;
        }
        std::cout << oscilline::region_name(identity.value().region) << " "
                  << identity.value().serial << "  boot " << identity.value().boot_file << '\n';
        for (const oscilline::FsNode& node : volume.value().nodes()) {
            std::cout << node.path;
            if (node.directory) {
                std::cout << "  directory";
            } else {
                std::cout << "  " << node.size << " bytes";
            }
            std::cout << "  lba " << node.lba;
            if (node.xa.present) {
                std::cout << "  xa 0x" << std::hex << node.xa.attributes << std::dec;
            }
            std::cout << '\n';
        }
        return EXIT_SUCCESS;
    }

    if (command != "extract") {
        print_usage();
        return EXIT_FAILURE;
    }
    if (argc != 4 && argc != 5) {
        print_usage();
        return EXIT_FAILURE;
    }
    const std::filesystem::path outdir{argv[3]};
    std::error_code error;
    std::filesystem::create_directories(outdir, error);
    if (error) {
        return refuse("cannot create '" + outdir.string() + "'");
    }

    const std::string only = argc == 5 ? argv[4] : "";
    const oscilline::FsNode* selected = nullptr;
    if (!only.empty()) {
        selected = volume.value().find(only);
        if (selected == nullptr) {
            return refuse("path not found in the image: " + only);
        }
    }

    bool skipped_form2 = false;
    for (const oscilline::FsNode& node : volume.value().nodes()) {
        if (node.path == "/") {
            continue;
        }
        if (selected != nullptr) {
            const std::string& wanted = selected->path;
            const bool self = node.path == wanted;
            const bool child = node.path.size() > wanted.size() &&
                               node.path.compare(0, wanted.size(), wanted) == 0 &&
                               node.path[wanted.size()] == '/';
            if (!self && !child) {
                continue;
            }
        }
        const auto destination = relative_destination(outdir, node.path);
        if (destination.empty()) {
            return refuse("refusing to write outside the output directory: " + node.path);
        }
        if (node.outside_data_track) {
            std::cerr << "oscilline-extract: '" << node.path
                      << "' points past the data track and was not extracted\n";
            continue;
        }
        if (node.directory) {
            std::filesystem::create_directories(destination, error);
            if (error) {
                return refuse("cannot create '" + destination.string() + "'");
            }
            continue;
        }
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error) {
            return refuse("cannot create '" + destination.parent_path().string() + "'");
        }
        if (node.xa.is_form2()) {
            std::cerr << "oscilline-extract: '" << node.path
                      << "' is a form 2 file and was not extracted\n";
            skipped_form2 = true;
            continue;
        }
        auto bytes = volume.value().read_file(node);
        if (!bytes) {
            if (bytes.error().find("not a 2048-byte data sector") != std::string::npos) {
                std::cerr << "oscilline-extract: '" << node.path << "': " << bytes.error() << '\n';
                skipped_form2 = true;
                continue;
            }
            return refuse(bytes.error());
        }
        std::ofstream out(destination, std::ios::binary);
        if (!out) {
            return refuse("cannot write '" + destination.string() + "'");
        }
        out.write(reinterpret_cast<const char*>(bytes.value().data()),
                  static_cast<std::streamsize>(bytes.value().size()));
        if (!out) {
            return refuse("failed while writing '" + destination.string() + "'");
        }
    }
    if (skipped_form2) {
        return refuse("one or more form 2 files were not extracted");
    }
    std::cout << "Extracted " << identity.value().serial << " into " << outdir.string() << '\n';
    return EXIT_SUCCESS;
}
