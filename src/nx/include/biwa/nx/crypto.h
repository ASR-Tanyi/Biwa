// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "biwa/core/error.h"

namespace biwa::nx {

using Aes128Key = std::array<std::uint8_t, 16>;
using AesXtsKey = std::array<std::uint8_t, 32>;  // data key, then tweak key
using AesCounter = std::array<std::uint8_t, 16>;

// AES-128 in ECB mode, in place. The length must be a multiple of 16.
Status AesEcbDecrypt(const Aes128Key& key, std::span<std::byte> data);
Status AesEcbEncrypt(const Aes128Key& key, std::span<std::byte> data);

// AES-128 in CTR mode, in place; `counter` is the big-endian counter block for the first byte of
// `data`, which must start on a 16-byte block. Encryption and decryption are the same.
void AesCtrCrypt(const Aes128Key& key, const AesCounter& counter, std::span<std::byte> data);

// The counter block for `offset` in a section whose upper 8 counter bytes are `upper`:
// upper (big-endian) followed by offset / 16 (big-endian), as NCA sections use it.
AesCounter NcaCounter(std::uint64_t upper, std::uint64_t offset);

// AES-128-XTS as Nintendo uses it: each sector's tweak is its index as a big-endian 128-bit
// number (IEEE 1619 uses little-endian). The length must be a multiple of `sector_size`.
Status AesXtsDecrypt(const AesXtsKey& key, std::span<std::byte> data, std::uint64_t first_sector,
                     std::size_t sector_size);
Status AesXtsEncrypt(const AesXtsKey& key, std::span<std::byte> data, std::uint64_t first_sector,
                     std::size_t sector_size);

}  // namespace biwa::nx
