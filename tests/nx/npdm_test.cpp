// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/npdm.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "support/pfs0_builder.h"

namespace {

using biwa::Errc;
using biwa::test::PutU32;
using biwa::test::PutU64;

// META at 0, ACI0 at 0x80 (kernel capabilities at +0x40), ACID at 0x100.
std::vector<std::byte> BuildNpdm(const std::vector<std::uint32_t>& caps) {
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
    PutU64(out, 0x80 + 0x10, 0x0100000000002000);
    PutU32(out, 0x80 + 0x30, 0x40);
    PutU32(out, 0x80 + 0x34, static_cast<std::uint32_t>(caps.size() * 4));
    for (std::size_t i = 0; i < caps.size(); ++i) {
        PutU32(out, 0x80 + 0x40 + i * 4, caps[i]);
    }

    std::memcpy(out.data() + 0x100 + 0x200, "ACID", 4);
    PutU64(out, 0x100 + 0x210, 0x0100000000002000);
    PutU64(out, 0x100 + 0x218, 0x01000000000020FF);
    return out;
}

// EnableSystemCalls for `group` with the given bits.
std::uint32_t Svcs(std::uint32_t group, std::uint32_t bits) { return (group << 29) | (bits << 5) | 0xF; }

TEST(NpdmTest, ReadsMetaAndCapabilities) {
    const std::uint32_t thread_info = (3u << 24) | (0u << 16) | (28u << 10) | (59u << 4) | 0x7;
    const std::uint32_t kernel_version = (0x30u << 15) | 0x3FFF;
    const std::uint32_t handles = (1023u << 16) | 0x7FFF;
    const auto npdm =
        biwa::nx::ParseNpdm(BuildNpdm({thread_info, Svcs(0, 0b1010), Svcs(1, 1), kernel_version, handles, 0xFFFFFFFF}));
    ASSERT_TRUE(npdm.has_value()) << npdm.error();
    EXPECT_EQ(npdm->name, "Application");
    EXPECT_TRUE(npdm->is_64bit);
    EXPECT_EQ(npdm->address_space_type, 3);
    EXPECT_EQ(npdm->main_thread_priority, 44);
    EXPECT_EQ(npdm->main_thread_stack_size, 0x100000u);
    EXPECT_EQ(npdm->system_resource_size, 0x100000u);
    EXPECT_EQ(npdm->program_id, 0x0100000000002000u);
    EXPECT_EQ(npdm->program_id_max, 0x01000000000020FFu);

    const biwa::nx::KernelCapabilities& caps = npdm->capabilities;
    ASSERT_TRUE(caps.thread_info.has_value());
    const auto info = caps.thread_info.value_or(biwa::nx::KernelCapabilities::ThreadInfo{});
    EXPECT_EQ(info.lowest_priority, 59);
    EXPECT_EQ(info.highest_priority, 28);
    EXPECT_EQ(info.min_core, 0);
    EXPECT_EQ(info.max_core, 3);
    EXPECT_EQ(caps.svcs.count(), 3u);
    EXPECT_TRUE(caps.svcs.test(1));
    EXPECT_TRUE(caps.svcs.test(3));
    EXPECT_TRUE(caps.svcs.test(24));
    EXPECT_EQ(caps.kernel_version.value_or(0), 0x30u);
    EXPECT_EQ(caps.handle_table_size.value_or(0), 1023u);
    EXPECT_FALSE(caps.program_type.has_value());
    EXPECT_EQ(caps.descriptors.size(), 6u);
}

TEST(NpdmTest, RejectsWhatDoesNotFit) {
    EXPECT_EQ(biwa::nx::ParseNpdm(std::vector<std::byte>(0x80)).error().code(), Errc::kInvalidArgument);
    std::vector<std::byte> bytes = BuildNpdm({});
    PutU32(bytes, 0x74, 0x1000);  // ACI0 size past the end
    EXPECT_EQ(biwa::nx::ParseNpdm(bytes).error().code(), Errc::kCorrupt);
    bytes = BuildNpdm({});
    bytes[0x100 + 0x200] = std::byte{'X'};
    EXPECT_EQ(biwa::nx::ParseNpdm(bytes).error().code(), Errc::kCorrupt);
}

}  // namespace
