// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/sha256.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "biwa/core/hex.h"

namespace {

std::span<const std::byte> AsBytes(std::string_view text) { return std::as_bytes(std::span(text)); }

std::string HexOf(std::string_view text) { return biwa::ToHex(biwa::Sha256Of(AsBytes(text))); }

// FIPS 180-4 example vectors.
TEST(Sha256Test, KnownVectors) {
    EXPECT_EQ(HexOf(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(HexOf("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, MillionAs) {
    const std::string text(1000000, 'a');
    EXPECT_EQ(HexOf(text), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, IncrementalMatchesOneShotAndFinishRestarts) {
    const std::string text = "The quick brown fox jumps over the lazy dog";
    biwa::Sha256 sha;
    for (std::size_t i = 0; i < text.size(); i += 5) {
        sha.Update(AsBytes(std::string_view(text).substr(i, 5)));
    }
    EXPECT_EQ(sha.Finish(), biwa::Sha256Of(AsBytes(text)));
    sha.Update(AsBytes("abc"));
    EXPECT_EQ(biwa::ToHex(sha.Finish()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HexTest, RoundTrips) {
    const std::vector<std::uint8_t> bytes = {0x00, 0x01, 0x7f, 0x80, 0xab, 0xff};
    EXPECT_EQ(biwa::ToHex(bytes), "00017f80abff");
    const auto parsed = biwa::ParseHex("00017F80abFF");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, bytes);
    EXPECT_TRUE(biwa::ParseHex("").has_value());
}

TEST(HexTest, RejectsBadTextWithoutEchoingIt) {
    const auto odd = biwa::ParseHex("abc");
    ASSERT_FALSE(odd.has_value());
    EXPECT_EQ(odd.error().code(), biwa::Errc::kInvalidArgument);
    const auto bad = biwa::ParseHex("00zz");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().message(), "not a hex digit at position 2");
}

}  // namespace
