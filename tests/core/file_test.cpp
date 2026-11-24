// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/file.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using biwa::Errc;
using biwa::InputFile;
using biwa::OutputFile;

std::vector<std::byte> Pattern(std::size_t size, std::uint32_t seed) {
    std::vector<std::byte> bytes(size);
    std::uint32_t x = seed;
    for (std::byte& b : bytes) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::byte>(x >> 24);
    }
    return bytes;
}

std::span<const std::byte> AsBytes(std::string_view text) { return std::as_bytes(std::span(text)); }

// A fresh directory per test, removed afterwards.
class FileTest : public ::testing::Test {
   protected:
    void SetUp() override {
        std::random_device random;
        dir_ = fs::temp_directory_path() / ("biwa-file-test-" + std::to_string(random()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_;
};

TEST_F(FileTest, WritesAndReadsBackAtOffsets) {
    const std::vector<std::byte> data = Pattern(100000, 1);
    {
        auto out = OutputFile::Create(dir_ / "a.bin");
        ASSERT_TRUE(out.has_value()) << out.error();
        // Second half first, to check offsets are honoured.
        ASSERT_TRUE(out->WriteAt(50000, std::span(data).subspan(50000)).has_value());
        ASSERT_TRUE(out->WriteAt(0, std::span(data).first(50000)).has_value());
        ASSERT_TRUE(out->Close().has_value());
    }
    auto in = InputFile::Open(dir_ / "a.bin");
    ASSERT_TRUE(in.has_value()) << in.error();
    EXPECT_EQ(in->Size(), data.size());
    std::vector<std::byte> back(1000);
    ASSERT_TRUE(in->ReadAt(12345, back).has_value());
    EXPECT_TRUE(std::equal(back.begin(), back.end(), data.begin() + 12345));
    ASSERT_TRUE(in->ReadAt(in->Size(), std::span<std::byte>()).has_value());  // empty read at the end
}

TEST_F(FileTest, ReadPastTheEndIsCorrupt) {
    ASSERT_TRUE(biwa::WriteFileAtomic(dir_ / "small.bin", AsBytes("0123456789")).has_value());
    auto in = InputFile::Open(dir_ / "small.bin");
    ASSERT_TRUE(in.has_value());
    std::array<std::byte, 4> buffer{};
    const auto past = in->ReadAt(8, buffer);
    ASSERT_FALSE(past.has_value());
    EXPECT_EQ(past.error().code(), Errc::kCorrupt);
    EXPECT_NE(past.error().message().find("ends past the end"), std::string::npos) << past.error();
    EXPECT_FALSE(in->ReadAt(~std::uint64_t{0}, buffer).has_value());  // offset overflow
}

TEST_F(FileTest, MissingFileIsNotFound) {
    const auto in = InputFile::Open(dir_ / "missing.bin");
    ASSERT_FALSE(in.has_value());
    EXPECT_EQ(in.error().code(), Errc::kNotFound);
    EXPECT_NE(in.error().message().find("missing.bin"), std::string::npos) << in.error();
    EXPECT_EQ(biwa::ReadFileBytes(dir_ / "missing.bin").error().code(), Errc::kNotFound);
    EXPECT_EQ(OutputFile::Create(dir_ / "no-dir" / "x.bin").error().code(), Errc::kNotFound);
}

TEST_F(FileTest, ConcurrentReadsSeeTheRightBytes) {
    const std::vector<std::byte> data = Pattern(1 << 20, 7);
    ASSERT_TRUE(biwa::WriteFileAtomic(dir_ / "big.bin", data).has_value());
    auto in = InputFile::Open(dir_ / "big.bin");
    ASSERT_TRUE(in.has_value());
    std::atomic<int> mismatches{0};
    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<std::byte> chunk(4096);
            for (std::size_t i = 0; i < 64; ++i) {
                const std::size_t offset =
                    ((i * 8 + static_cast<std::size_t>(t)) * 4093) % (data.size() - chunk.size());
                if (!in->ReadAt(offset, chunk).has_value() ||
                    !std::equal(chunk.begin(), chunk.end(), data.begin() + static_cast<std::ptrdiff_t>(offset))) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(FileTest, SetSizeExtendsWithZerosAndTruncates) {
    {
        auto out = OutputFile::Create(dir_ / "sized.bin");
        ASSERT_TRUE(out.has_value());
        ASSERT_TRUE(out->WriteAt(0, AsBytes("abc")).has_value());
        ASSERT_TRUE(out->SetSize(4096).has_value());
        ASSERT_TRUE(out->Close().has_value());
    }
    auto bytes = biwa::ReadFileBytes(dir_ / "sized.bin");
    ASSERT_TRUE(bytes.has_value());
    ASSERT_EQ(bytes->size(), 4096u);
    EXPECT_EQ((*bytes)[2], std::byte{'c'});
    EXPECT_EQ((*bytes)[4095], std::byte{0});
    {
        auto out = OutputFile::Create(dir_ / "sized.bin");  // emptied on create
        ASSERT_TRUE(out.has_value());
        ASSERT_TRUE(out->Close().has_value());
    }
    EXPECT_EQ(fs::file_size(dir_ / "sized.bin"), 0u);
}

TEST_F(FileTest, WriteAfterCloseIsAnInternalError) {
    auto out = OutputFile::Create(dir_ / "closed.bin");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->Close().has_value());
    ASSERT_TRUE(out->Close().has_value());  // closing twice is fine
    EXPECT_EQ(out->WriteAt(0, AsBytes("x")).error().code(), Errc::kInternal);
}

TEST_F(FileTest, AtomicWriteReplacesAndLeavesNoTemporary) {
    const fs::path path = dir_ / "manifest.json";
    ASSERT_TRUE(biwa::WriteFileAtomic(path, AsBytes("first")).has_value());
    ASSERT_TRUE(biwa::WriteFileAtomic(path, AsBytes("second, longer")).has_value());
    auto text = biwa::ReadFileText(path);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "second, longer");
    EXPECT_FALSE(fs::exists(dir_ / "manifest.json.tmp"));
}

TEST_F(FileTest, MovedFromFilesAreHarmless) {
    ASSERT_TRUE(biwa::WriteFileAtomic(dir_ / "m.bin", AsBytes("xyz")).has_value());
    auto first = InputFile::Open(dir_ / "m.bin");
    ASSERT_TRUE(first.has_value());
    InputFile second = std::move(*first);
    std::array<std::byte, 3> buffer{};
    ASSERT_TRUE(second.ReadAt(0, buffer).has_value());
    EXPECT_EQ(buffer[2], std::byte{'z'});
}

}  // namespace
