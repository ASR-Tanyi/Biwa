// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/hex.h"

namespace biwa {
namespace {

constexpr std::string_view kDigits = "0123456789abcdef";

int DigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string ToHex(std::span<const std::uint8_t> bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        text.push_back(kDigits[b >> 4]);
        text.push_back(kDigits[b & 0xF]);
    }
    return text;
}

Result<std::vector<std::uint8_t>> ParseHex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return Failf(Errc::kInvalidArgument, "hex text has an odd number of digits ({})", text.size());
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = DigitValue(text[i]);
        const int low = DigitValue(text[i + 1]);
        if (high < 0 || low < 0) {
            return Failf(Errc::kInvalidArgument, "not a hex digit at position {}", high < 0 ? i : i + 1);
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

}  // namespace biwa
