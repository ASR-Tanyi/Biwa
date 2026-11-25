// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/nso.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "support/nso_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::NsoSegmentKind;

biwa::test::NsoBuildOptions Options() {
    biwa::test::NsoBuildOptions options;
    options.segments[0] = biwa::test::TextWithMod0(0x900, 0x2100, 0x2300, 0x2800);
    options.segments[1] = biwa::test::Bytes("read-only data");
    options.segments[2] = std::vector<std::byte>(0x300, std::byte{0x5A});
    return options;
}

biwa::Result<biwa::nx::Nso> Read(const std::vector<std::byte>& bytes) {
    return biwa::nx::ReadNso(*biwa::nx::MakeMemoryStorage(bytes));
}

TEST(NsoTest, LoadsSegmentsAtTheirMemoryOffsets) {
    for (const bool compress : {true, false}) {
        biwa::test::NsoBuildOptions options = Options();
        options.compress = compress;
        const auto nso = Read(biwa::test::BuildNso(options));
        ASSERT_TRUE(nso.has_value()) << nso.error();
        ASSERT_EQ(nso->image.size(), 0x2300u);
        EXPECT_TRUE(std::equal(options.segments[0].begin(), options.segments[0].end(), nso->image.begin()));
        EXPECT_TRUE(std::all_of(nso->image.begin() + 0x900, nso->image.begin() + 0x1000,
                                [](std::byte b) { return b == std::byte{0}; }));
        EXPECT_EQ(nso->image[0x1000], std::byte{'r'});
        EXPECT_EQ(nso->image[0x22FF], std::byte{0x5A});
        EXPECT_EQ(nso->segment(NsoSegmentKind::kRodata).memory_offset, 0x1000u);
        EXPECT_EQ(nso->segment(NsoSegmentKind::kData).size, 0x300u);
        EXPECT_EQ(nso->segment(NsoSegmentKind::kText).compressed, compress);
        EXPECT_EQ(nso->bss_size, 0x500u);
        EXPECT_EQ(nso->build_id[0], 0x11);
        EXPECT_EQ(nso->build_id[0x1F], 0x30);
    }
}

TEST(NsoTest, ReadsMod0) {
    const auto nso = Read(biwa::test::BuildNso(Options()));
    ASSERT_TRUE(nso.has_value()) << nso.error();
    ASSERT_TRUE(nso->mod0.has_value());
    const biwa::nx::Mod0 mod0 = nso->mod0.value_or(biwa::nx::Mod0{});
    EXPECT_EQ(mod0.offset, 8u);
    EXPECT_EQ(mod0.dynamic, 0x2100u);
    EXPECT_EQ(mod0.bss_start, 0x2300u);
    EXPECT_EQ(mod0.bss_end, 0x2800u);
    EXPECT_EQ(mod0.eh_frame_hdr_start, 0x1800u);
    EXPECT_EQ(mod0.eh_frame_hdr_end, 0x1820u);

    biwa::test::NsoBuildOptions options = Options();
    options.segments[0][8] = std::byte{'X'};
    const auto without = Read(biwa::test::BuildNso(options));
    ASSERT_TRUE(without.has_value()) << without.error();
    EXPECT_FALSE(without->mod0.has_value());
}

TEST(NsoTest, ChecksHashesAndLayout) {
    std::vector<std::byte> bytes = biwa::test::BuildNso(Options());
    bytes[0xC0] ^= std::byte{1};  // the rodata hash
    auto nso = Read(bytes);
    ASSERT_FALSE(nso.has_value());
    EXPECT_EQ(nso.error().code(), Errc::kCorrupt);
    EXPECT_NE(nso.error().message().find("rodata"), std::string::npos);

    biwa::test::NsoBuildOptions options = Options();
    options.memory_offsets = {0, 0x800, 0x2000};  // rodata starts inside text
    EXPECT_EQ(Read(biwa::test::BuildNso(options)).error().code(), Errc::kCorrupt);

    bytes = biwa::test::BuildNso(Options());
    bytes.resize(0x140);  // segments cut off
    EXPECT_EQ(Read(bytes).error().code(), Errc::kCorrupt);

    bytes = biwa::test::BuildNso(Options());
    bytes[0] = std::byte{'X'};
    EXPECT_EQ(Read(bytes).error().code(), Errc::kInvalidArgument);
    EXPECT_EQ(Read(std::vector<std::byte>(0x20)).error().code(), Errc::kInvalidArgument);
}

TEST(NsoTest, ADamagedCompressedSegmentIsCorrupt) {
    biwa::test::NsoBuildOptions options = Options();
    options.hash = false;
    std::vector<std::byte> bytes = biwa::test::BuildNso(options);
    bytes[0x100] = std::byte{0xFF};  // the first LZ4 token of .text
    bytes[0x101] = std::byte{0xFF};
    EXPECT_EQ(Read(bytes).error().code(), Errc::kCorrupt);
}

}  // namespace
