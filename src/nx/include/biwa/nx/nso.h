// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/core/sha256.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// Largest module image Biwa loads (text, rodata and data together).
inline constexpr std::uint64_t kMaxNsoImageSize = std::uint64_t{1} << 30;

enum class NsoSegmentKind : std::uint8_t { kText, kRodata, kData };

struct NsoSegment {
    std::uint32_t file_offset = 0;
    std::uint32_t file_size = 0;      // as stored (compressed size when compressed)
    std::uint32_t memory_offset = 0;  // from the module's base
    std::uint32_t size = 0;           // in memory
    bool compressed = false;          // LZ4 block
    bool hashed = false;              // `hash` is checked when the segment is read
    Sha256Digest hash{};              // SHA-256 of the segment in memory
};

// The MOD0 header, with every offset made relative to the module's base.
struct Mod0 {
    std::uint32_t offset = 0;
    std::uint32_t dynamic = 0;
    std::uint32_t bss_start = 0;
    std::uint32_t bss_end = 0;
    std::uint32_t eh_frame_hdr_start = 0;
    std::uint32_t eh_frame_hdr_end = 0;
    std::uint32_t module_object = 0;
};

// An NSO module: its header and its memory image (text, rodata and data at their memory offsets,
// zeros between them; .bss is not included).
struct Nso {
    std::array<std::uint8_t, 0x20> build_id{};
    std::array<NsoSegment, 3> segments{};  // by NsoSegmentKind
    std::uint32_t bss_size = 0;
    std::vector<std::byte> image;
    std::optional<Mod0> mod0;  // absent if the image has none

    [[nodiscard]] const NsoSegment& segment(NsoSegmentKind kind) const {
        return segments[static_cast<std::size_t>(kind)];
    }
};

// Reads an NSO file: checks the header, decompresses the segments and checks their hashes.
// Errc::kInvalidArgument if it is not an NSO; Errc::kCorrupt for a bad header, a segment that does
// not decompress to its size, or a hash that does not match.
Result<Nso> ReadNso(const Storage& file);

}  // namespace biwa::nx
