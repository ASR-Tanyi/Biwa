// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace biwa::test {

struct BuildFile {
    std::string name;
    std::vector<std::byte> data;
};

inline void PutU32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}
inline void PutU64(std::vector<std::byte>& out, std::size_t at, std::uint64_t value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

// Builds a PFS0 container the way Nintendo's tools do: names packed in order, the string table
// padded so the data starts on a 0x20 boundary, files back to back.
inline std::vector<std::byte> BuildPfs0(const std::vector<BuildFile>& files) {
    std::string strings;
    std::vector<std::uint32_t> name_offsets;
    for (const BuildFile& file : files) {
        name_offsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings += file.name;
        strings.push_back('\0');
    }
    const std::size_t table_end = 0x10 + files.size() * 0x18 + strings.size();
    strings.resize(strings.size() + ((0x20 - table_end % 0x20) % 0x20), '\0');
    const std::size_t data_start = 0x10 + files.size() * 0x18 + strings.size();

    std::size_t data_size = 0;
    for (const BuildFile& file : files) {
        data_size += file.data.size();
    }
    std::vector<std::byte> out(data_start + data_size);
    PutU32(out, 0, 0x30534650);  // "PFS0"
    PutU32(out, 4, static_cast<std::uint32_t>(files.size()));
    PutU32(out, 8, static_cast<std::uint32_t>(strings.size()));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t entry = 0x10 + i * 0x18;
        PutU64(out, entry, offset);
        PutU64(out, entry + 8, files[i].data.size());
        PutU32(out, entry + 16, name_offsets[i]);
        std::memcpy(out.data() + data_start + offset, files[i].data.data(), files[i].data.size());
        offset += files[i].data.size();
    }
    std::memcpy(out.data() + 0x10 + files.size() * 0x18, strings.data(), strings.size());
    return out;
}

inline std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

}  // namespace biwa::test
