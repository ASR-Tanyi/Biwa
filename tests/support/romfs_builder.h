// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "support/pfs0_builder.h"

namespace biwa::test {

struct RomfsBuildFile {
    std::string path;  // "a/b/c.bin"
    std::vector<std::byte> data;
};

// Builds a RomFS image: header, file data at 0x200 (each file 16-byte aligned), then the
// directory and file tables. Directories come from the paths; entries keep the order given. The
// hash tables have one empty bucket (Biwa's reader walks the tree instead).
inline std::vector<std::byte> BuildRomfs(const std::vector<RomfsBuildFile>& files) {
    constexpr std::uint32_t kNone = 0xFFFFFFFF;
    struct Dir {
        std::string name;
        std::size_t parent = 0;
        std::vector<std::size_t> dirs;
        std::vector<std::size_t> files;
        std::uint32_t offset = 0;
    };
    struct File {
        std::string name;
        std::size_t parent = 0;
        std::size_t source = 0;
        std::uint32_t offset = 0;
        std::uint64_t data_offset = 0;
    };
    std::vector<Dir> dirs(1);
    std::vector<File> entries;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::size_t dir = 0;
        std::string_view rest = files[i].path;
        for (std::size_t slash = rest.find('/'); slash != std::string_view::npos; slash = rest.find('/')) {
            const std::string name(rest.substr(0, slash));
            rest.remove_prefix(slash + 1);
            std::size_t next = 0;
            for (const std::size_t child : dirs[dir].dirs) {
                if (dirs[child].name == name) {
                    next = child;
                }
            }
            if (next == 0) {
                next = dirs.size();
                dirs[dir].dirs.push_back(next);
                dirs.push_back({name, dir, {}, {}, 0});
            }
            dir = next;
        }
        dirs[dir].files.push_back(entries.size());
        entries.push_back({std::string(rest), dir, i, 0, 0});
    }

    auto padded = [](std::size_t n) { return (n + 3) / 4 * 4; };
    std::uint32_t dir_size = 0;
    for (Dir& dir : dirs) {  // creation order: parents before children
        dir.offset = dir_size;
        dir_size += static_cast<std::uint32_t>(0x18 + padded(dir.name.size()));
    }
    std::uint32_t file_size = 0;
    std::uint64_t data_size = 0;
    for (const Dir& dir : dirs) {
        for (const std::size_t f : dir.files) {
            entries[f].offset = file_size;
            file_size += static_cast<std::uint32_t>(0x20 + padded(entries[f].name.size()));
            entries[f].data_offset = data_size;
            data_size = (data_size + files[entries[f].source].data.size() + 15) / 16 * 16;
        }
    }

    const std::size_t data_start = 0x200;
    const std::size_t dir_hash = data_start + data_size;
    const std::size_t dir_table = dir_hash + 4;
    const std::size_t file_hash = dir_table + dir_size;
    const std::size_t file_table = file_hash + 4;
    std::vector<std::byte> out(file_table + file_size);
    const std::uint64_t header[10] = {0x50, dir_hash, 4, dir_table, dir_size, file_hash, 4, file_table, file_size,
                                      data_start};
    std::memcpy(out.data(), header, sizeof(header));
    PutU32(out, dir_hash, kNone);
    PutU32(out, file_hash, kNone);

    auto sibling_of = [](const std::vector<std::size_t>& list, std::size_t i, auto offset_of) {
        return i + 1 < list.size() ? offset_of(list[i + 1]) : kNone;
    };
    auto dir_offset = [&](std::size_t d) { return dirs[d].offset; };
    auto file_offset = [&](std::size_t f) { return entries[f].offset; };
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        const Dir& dir = dirs[d];
        const std::vector<std::size_t>& siblings = dirs[dir.parent].dirs;
        std::uint32_t sibling = kNone;
        for (std::size_t i = 0; d != 0 && i < siblings.size(); ++i) {
            if (siblings[i] == d) {
                sibling = sibling_of(siblings, i, dir_offset);
            }
        }
        const std::size_t at = dir_table + dir.offset;
        PutU32(out, at + 0x00, dirs[dir.parent].offset);
        PutU32(out, at + 0x04, sibling);
        PutU32(out, at + 0x08, dir.dirs.empty() ? kNone : dirs[dir.dirs[0]].offset);
        PutU32(out, at + 0x0C, dir.files.empty() ? kNone : entries[dir.files[0]].offset);
        PutU32(out, at + 0x10, kNone);
        PutU32(out, at + 0x14, static_cast<std::uint32_t>(dir.name.size()));
        std::memcpy(out.data() + at + 0x18, dir.name.data(), dir.name.size());
        for (std::size_t i = 0; i < dir.files.size(); ++i) {
            const File& file = entries[dir.files[i]];
            const std::vector<std::byte>& data = files[file.source].data;
            const std::size_t fat = file_table + file.offset;
            PutU32(out, fat + 0x00, dir.offset);
            PutU32(out, fat + 0x04, sibling_of(dir.files, i, file_offset));
            PutU64(out, fat + 0x08, file.data_offset);
            PutU64(out, fat + 0x10, data.size());
            PutU32(out, fat + 0x18, kNone);
            PutU32(out, fat + 0x1C, static_cast<std::uint32_t>(file.name.size()));
            std::memcpy(out.data() + fat + 0x20, file.name.data(), file.name.size());
            std::memcpy(out.data() + data_start + file.data_offset, data.data(), data.size());
        }
    }
    return out;
}

}  // namespace biwa::test
