// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/romfs.h"

#include <gtest/gtest.h>

#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/romfs_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::Romfs;
using biwa::test::Bytes;

std::vector<std::byte> Sample() {
    return biwa::test::BuildRomfs({
        {"Pack/Bootup.pack", Bytes("SARC bootup")},
        {"Pack/Title.pack", Bytes("SARC title")},
        {"System/Version.txt", Bytes("1.0.0")},
        {"System/Resource/ResourceSizeTable.product.rsizetable", Bytes("RSTB")},
        {"top.bin", Bytes("top level file")},
    });
}

Romfs Open(std::vector<std::byte> bytes) {
    auto romfs = Romfs::Open(biwa::nx::MakeMemoryStorage(std::move(bytes)));
    EXPECT_TRUE(romfs.has_value()) << romfs.error();
    return std::move(*romfs);
}

std::string Text(const biwa::nx::Storage& storage) {
    std::string text(static_cast<std::size_t>(storage.Size()), '\0');
    EXPECT_TRUE(storage.Read(0, std::as_writable_bytes(std::span(text))).has_value());
    return text;
}

TEST(RomfsTest, ListsDirectoriesThenFiles) {
    const Romfs romfs = Open(Sample());
    for (const std::string_view root : {"", "/"}) {
        const auto entries = romfs.List(root);
        ASSERT_TRUE(entries.has_value()) << entries.error();
        ASSERT_EQ(entries->size(), 3u);
        EXPECT_EQ((*entries)[0].name, "Pack");
        EXPECT_TRUE((*entries)[0].is_directory);
        EXPECT_EQ((*entries)[1].name, "System");
        EXPECT_EQ((*entries)[2].name, "top.bin");
        EXPECT_FALSE((*entries)[2].is_directory);
        EXPECT_EQ((*entries)[2].size, 14u);
    }
    const auto system = romfs.List("/System/");
    ASSERT_TRUE(system.has_value()) << system.error();
    ASSERT_EQ(system->size(), 2u);
    EXPECT_EQ((*system)[0].name, "Resource");
    EXPECT_EQ((*system)[1].name, "Version.txt");
}

TEST(RomfsTest, OpensFilesByPath) {
    const Romfs romfs = Open(Sample());
    const auto file = romfs.OpenFile("Pack/Title.pack");
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_EQ(Text(**file), "SARC title");
    const auto deep = romfs.OpenFile("/System/Resource/ResourceSizeTable.product.rsizetable");
    ASSERT_TRUE(deep.has_value()) << deep.error();
    EXPECT_EQ(Text(**deep), "RSTB");

    EXPECT_EQ(romfs.OpenFile("Pack/Missing.pack").error().code(), Errc::kNotFound);
    EXPECT_EQ(romfs.OpenFile("top.bin/x").error().code(), Errc::kNotFound);
    EXPECT_EQ(romfs.OpenFile("Pack").error().code(), Errc::kInvalidArgument);
    EXPECT_EQ(romfs.List("top.bin").error().code(), Errc::kInvalidArgument);
}

TEST(RomfsTest, VisitsEveryFileWithItsPath) {
    const Romfs romfs = Open(Sample());
    std::vector<std::pair<std::string, std::uint64_t>> seen;
    const auto status = romfs.ForEachFile([&](std::string_view path, std::uint64_t size) -> biwa::Status {
        seen.emplace_back(path, size);
        return {};
    });
    ASSERT_TRUE(status.has_value()) << status.error();
    const std::vector<std::pair<std::string, std::uint64_t>> expected = {
        {"/top.bin", 14},
        {"/Pack/Bootup.pack", 11},
        {"/Pack/Title.pack", 10},
        {"/System/Version.txt", 5},
        {"/System/Resource/ResourceSizeTable.product.rsizetable", 4},
    };
    EXPECT_EQ(seen, expected);
}

TEST(RomfsTest, AnEmptyRomfsHasNoEntries) {
    const Romfs romfs = Open(biwa::test::BuildRomfs({}));
    const auto entries = romfs.List("/");
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(entries->empty());
}

// Offsets into Sample()'s tables, from its header.
std::uint64_t HeaderField(const std::vector<std::byte>& bytes, std::size_t index) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + index * 8, 8);
    return value;
}

TEST(RomfsTest, RejectsUnsafeNames) {
    for (const std::string_view bad : {"..", ".", "a\\b"}) {
        std::vector<std::byte> bytes = biwa::test::BuildRomfs({{"xx/f.bin", Bytes("data")}});
        // The first directory after the root is "xx": overwrite its name.
        const std::size_t name = HeaderField(bytes, 3) + 0x18 + 0x18;
        biwa::test::PutU32(bytes, name - 4, static_cast<std::uint32_t>(bad.size()));
        std::memcpy(bytes.data() + name, bad.data(), bad.size());
        const Romfs romfs = Open(bytes);
        EXPECT_EQ(romfs.List("/").error().code(), Errc::kCorrupt) << bad;
        EXPECT_EQ(romfs.ForEachFile([](std::string_view, std::uint64_t) -> biwa::Status { return {}; })
                      .error()
                      .code(),
                  Errc::kCorrupt)
            << bad;
    }
}

TEST(RomfsTest, RejectsLoopsAndBadTables) {
    std::vector<std::byte> bytes = biwa::test::BuildRomfs({{"a/f.bin", Bytes("data")}});
    // Make "a" its own sibling.
    const std::size_t a = HeaderField(bytes, 3) + 0x18;
    biwa::test::PutU32(bytes, a + 4, 0x18);
    const Romfs looped = Open(bytes);
    EXPECT_EQ(looped.List("/").error().code(), Errc::kCorrupt);
    EXPECT_EQ(looped.ForEachFile([](std::string_view, std::uint64_t) -> biwa::Status { return {}; }).error().code(),
              Errc::kCorrupt);

    bytes = biwa::test::BuildRomfs({{"f.bin", Bytes("data")}});
    biwa::test::PutU64(bytes, 0x20, 0x10000);  // directory table size past the end
    EXPECT_EQ(Romfs::Open(biwa::nx::MakeMemoryStorage(bytes)).error().code(), Errc::kCorrupt);

    bytes = biwa::test::BuildRomfs({{"f.bin", Bytes("data")}});
    biwa::test::PutU64(bytes, 0, 0x40);  // header size
    EXPECT_EQ(Romfs::Open(biwa::nx::MakeMemoryStorage(bytes)).error().code(), Errc::kCorrupt);
    EXPECT_EQ(Romfs::Open(biwa::nx::MakeMemoryStorage(std::vector<std::byte>(0x20))).error().code(), Errc::kCorrupt);

    bytes = biwa::test::BuildRomfs({{"f.bin", Bytes("data")}});
    const std::size_t file = HeaderField(bytes, 7);
    biwa::test::PutU64(bytes, file + 0x10, 0x100000);  // data size past the end
    EXPECT_EQ(Open(bytes).OpenFile("f.bin").error().code(), Errc::kCorrupt);
}

}  // namespace
