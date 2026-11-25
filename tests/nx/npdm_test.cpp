// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/npdm.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "support/meta_builder.h"

namespace {

using biwa::Errc;
using biwa::test::BuildNpdm;
using biwa::test::PutU32;

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
