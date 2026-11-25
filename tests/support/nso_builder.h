// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <lz4.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "biwa/core/sha256.h"
#include "support/pfs0_builder.h"

namespace biwa::test {

struct NsoBuildOptions {
    std::array<std::vector<std::byte>, 3> segments;  // text, rodata, data
    std::array<std::uint32_t, 3> memory_offsets{0, 0x1000, 0x2000};
    std::uint32_t bss_size = 0x500;
    bool compress = true;
    bool hash = true;
    std::uint8_t build_id_seed = 0x11;
};

// An NSO with the given segments, each LZ4-compressed (if `compress`) and hashed (if `hash`).
inline std::vector<std::byte> BuildNso(const NsoBuildOptions& options) {
    std::vector<std::byte> out(0x100);
    std::memcpy(out.data(), "NSO0", 4);
    std::uint32_t flags = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::vector<std::byte>& segment = options.segments[i];
        std::vector<std::byte> stored = segment;
        if (options.compress) {
            std::vector<char> packed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(segment.size()))));
            const int n = LZ4_compress_default(reinterpret_cast<const char*>(segment.data()), packed.data(),
                                               static_cast<int>(segment.size()), static_cast<int>(packed.size()));
            const auto* bytes = reinterpret_cast<const std::byte*>(packed.data());
            stored.assign(bytes, bytes + n);
            flags |= 1u << i;
            PutU32(out, 0x60 + i * 4, static_cast<std::uint32_t>(stored.size()));
        }
        if (options.hash) {
            flags |= 1u << (i + 3);
            const Sha256Digest digest = Sha256Of(segment);
            std::memcpy(out.data() + 0xA0 + i * 0x20, digest.data(), digest.size());
        }
        PutU32(out, 0x10 + i * 0x10, static_cast<std::uint32_t>(out.size()));
        PutU32(out, 0x14 + i * 0x10, options.memory_offsets[i]);
        PutU32(out, 0x18 + i * 0x10, static_cast<std::uint32_t>(segment.size()));
        out.insert(out.end(), stored.begin(), stored.end());
    }
    PutU32(out, 0x0C, flags);
    PutU32(out, 0x3C, options.bss_size);
    for (std::size_t i = 0; i < 0x20; ++i) {
        out[0x40 + i] = static_cast<std::byte>(options.build_id_seed + i);
    }
    return out;
}

// A .text that starts like a real module's: a branch, then the offset of MOD0 at 8, then MOD0
// whose fields point at the given offsets from the module's base.
inline std::vector<std::byte> TextWithMod0(std::size_t size, std::uint32_t dynamic, std::uint32_t bss_start,
                                           std::uint32_t bss_end) {
    std::vector<std::byte> text(size);
    PutU32(text, 4, 8);
    std::memcpy(text.data() + 8, "MOD0", 4);
    auto relative = [](std::uint32_t target) { return static_cast<std::uint32_t>(target - 8); };
    PutU32(text, 8 + 0x04, relative(dynamic));
    PutU32(text, 8 + 0x08, relative(bss_start));
    PutU32(text, 8 + 0x0C, relative(bss_end));
    PutU32(text, 8 + 0x10, relative(0x1800));
    PutU32(text, 8 + 0x14, relative(0x1820));
    PutU32(text, 8 + 0x18, relative(bss_start));
    for (std::size_t i = 0x40; i < size; ++i) {
        text[i] = static_cast<std::byte>(i % 7);
    }
    return text;
}

}  // namespace biwa::test
