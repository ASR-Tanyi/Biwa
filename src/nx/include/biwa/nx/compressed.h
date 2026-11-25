// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "biwa/core/error.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

enum class CompressionType : std::uint8_t { kNone = 0, kZeros = 1, kLz4 = 3 };

// The decompressed bytes of a compressed section. `stored` is the section's file system level
// (decrypted and hash-checked); the compression info (0x28 bytes at 0x178 in the FS header) gives
// a bucket tree at an offset in it, whose entries map virtual ranges to stored ranges before the
// tree: copied as they are, zeros, or LZ4 blocks. The result's size is the tree's end offset.
// Errc::kCorrupt for a bad tree or a block that does not decompress to its size;
// Errc::kUnsupported for an unknown type.
Result<StoragePtr> MakeCompressedStorage(StoragePtr stored, const std::array<std::byte, 0x28>& compression_info);

}  // namespace biwa::nx
