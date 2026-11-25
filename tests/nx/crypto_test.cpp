// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/crypto.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "biwa/core/hex.h"
#include "biwa/core/sha256.h"

namespace {

using biwa::nx::Aes128Key;
using biwa::nx::AesXtsKey;

std::vector<std::byte> FromHex(std::string_view text) {
    const auto bytes = biwa::ParseHex(text);
    EXPECT_TRUE(bytes.has_value());
    std::vector<std::byte> out(bytes->size());
    std::memcpy(out.data(), bytes->data(), out.size());
    return out;
}

template <std::size_t N>
std::array<std::uint8_t, N> KeyFromHex(std::string_view text) {
    const auto bytes = biwa::ParseHex(text);
    std::array<std::uint8_t, N> key{};
    std::ranges::copy(*bytes, key.begin());
    return key;
}

std::string Hex(std::span<const std::byte> data) {
    return biwa::ToHex(std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

// FIPS-197 appendix C.1.
TEST(CryptoTest, EcbKnownAnswer) {
    const Aes128Key key = KeyFromHex<16>("000102030405060708090a0b0c0d0e0f");
    std::vector<std::byte> data = FromHex("00112233445566778899aabbccddeeff");
    ASSERT_TRUE(biwa::nx::AesEcbEncrypt(key, data).has_value());
    EXPECT_EQ(Hex(data), "69c4e0d86a7b0430d8cdb78070b4c55a");
    ASSERT_TRUE(biwa::nx::AesEcbDecrypt(key, data).has_value());
    EXPECT_EQ(Hex(data), "00112233445566778899aabbccddeeff");
    std::vector<std::byte> odd(15);
    EXPECT_FALSE(biwa::nx::AesEcbDecrypt(key, odd).has_value());
}

// NIST SP 800-38A F.5.1, first two blocks.
TEST(CryptoTest, CtrKnownAnswer) {
    const Aes128Key key = KeyFromHex<16>("2b7e151628aed2a6abf7158809cf4f3c");
    const auto counter = KeyFromHex<16>("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    std::vector<std::byte> data = FromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    biwa::nx::AesCtrCrypt(key, counter, data);
    EXPECT_EQ(Hex(data), "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff");
    biwa::nx::AesCtrCrypt(key, counter, data);
    EXPECT_EQ(Hex(data), "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
}

TEST(CryptoTest, NcaCounterIsUpperThenBlockIndex) {
    const auto counter = biwa::nx::NcaCounter(0x0000000100000002, 0x12340);
    EXPECT_EQ(biwa::ToHex(counter), "00000001000000020000000000001234");
}

// IEEE 1619 vector 1: zero keys, sector 0 (the tweak is all zeros in either byte order).
TEST(CryptoTest, XtsIeeeVectorOne) {
    const AesXtsKey key{};
    std::vector<std::byte> data(32);
    ASSERT_TRUE(biwa::nx::AesXtsEncrypt(key, data, 0, 32).has_value());
    EXPECT_EQ(Hex(data), "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
}

// Sectors 5 and 6 of 0x200 bytes with big-endian tweaks, as NCA headers use. The expected
// values come from Python's `cryptography` with the tweak bytes built the same way.
TEST(CryptoTest, XtsNintendoTweak) {
    AesXtsKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(i);
    }
    std::vector<std::byte> data(0x400);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i & 0xFF);
    }
    const std::vector<std::byte> plain = data;
    ASSERT_TRUE(biwa::nx::AesXtsEncrypt(key, data, 5, 0x200).has_value());
    EXPECT_EQ(Hex(std::span(data).first(16)), "bb6282966bcde70df1f603e50a5a4eba");
    EXPECT_EQ(Hex(std::span(data).subspan(0x200, 16)), "85887e76f59f011ac015da6e87514eb1");
    EXPECT_EQ(biwa::ToHex(biwa::Sha256Of(data)), "55e727a027c72c105b202ac5ccc3c1d85a4f4474a73572362ee36aeadd34d323");
    ASSERT_TRUE(biwa::nx::AesXtsDecrypt(key, data, 5, 0x200).has_value());
    EXPECT_EQ(data, plain);
    EXPECT_FALSE(biwa::nx::AesXtsDecrypt(key, std::span(data).first(0x300), 5, 0x200).has_value());
}

}  // namespace
