// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "support/pfs0_builder.h"

namespace biwa::test {

struct CnmtBuildContent {
    std::array<std::uint8_t, 0x10> id{};
    std::uint64_t size = 0;
    std::uint8_t type = 1;  // 0 meta, 1 program, 2 data, 3 control, ...
};

// A packaged content meta with a 0x10-byte extended header: {related title ID, required system
// version, required application version}.
inline std::vector<std::byte> BuildCnmt(std::uint64_t title_id, std::uint32_t version, std::uint8_t type,
                                        std::uint64_t related, const std::vector<CnmtBuildContent>& contents) {
    std::vector<std::byte> out(0x20 + 0x10 + contents.size() * 0x38 + 0x20);
    PutU64(out, 0x00, title_id);
    PutU32(out, 0x08, version);
    out[0x0C] = static_cast<std::byte>(type);
    out[0x0E] = std::byte{0x10};
    out[0x10] = static_cast<std::byte>(contents.size());
    PutU64(out, 0x20, related);
    PutU32(out, 0x28, 0x0C000000);
    PutU32(out, 0x2C, 0x10000);
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const std::size_t at = 0x30 + i * 0x38;
        out[at] = std::byte{0xAA};  // hash
        std::memcpy(out.data() + at + 0x20, contents[i].id.data(), 0x10);
        PutU32(out, at + 0x30, static_cast<std::uint32_t>(contents[i].size));
        out[at + 0x34] = static_cast<std::byte>(contents[i].size >> 32);
        out[at + 0x36] = static_cast<std::byte>(contents[i].type);
    }
    return out;
}

// main.npdm: META at 0, ACI0 at 0x80 (kernel capabilities at +0x40), ACID at 0x100.
inline std::vector<std::byte> BuildNpdm(const std::vector<std::uint32_t>& caps,
                                        std::uint64_t program_id = 0x0100000000002000) {
    std::vector<std::byte> out(0x400);
    std::memcpy(out.data(), "META", 4);
    out[0x0C] = std::byte{0x07};  // 64-bit, address space type 3
    out[0x0E] = std::byte{44};
    out[0x0F] = std::byte{0};
    PutU32(out, 0x14, 0x100000);
    PutU32(out, 0x18, 3);
    PutU32(out, 0x1C, 0x100000);
    std::memcpy(out.data() + 0x20, "Application", 11);
    PutU32(out, 0x70, 0x80);
    PutU32(out, 0x74, static_cast<std::uint32_t>(0x40 + caps.size() * 4));
    PutU32(out, 0x78, 0x100);
    PutU32(out, 0x7C, 0x240);

    std::memcpy(out.data() + 0x80, "ACI0", 4);
    PutU64(out, 0x80 + 0x10, program_id);
    PutU32(out, 0x80 + 0x30, 0x40);
    PutU32(out, 0x80 + 0x34, static_cast<std::uint32_t>(caps.size() * 4));
    for (std::size_t i = 0; i < caps.size(); ++i) {
        PutU32(out, 0x80 + 0x40 + i * 4, caps[i]);
    }

    std::memcpy(out.data() + 0x100 + 0x200, "ACID", 4);
    PutU64(out, 0x100 + 0x210, program_id);
    PutU64(out, 0x100 + 0x218, program_id | 0xFF);
    return out;
}

}  // namespace biwa::test
