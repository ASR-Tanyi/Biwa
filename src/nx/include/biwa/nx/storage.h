// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/core/file.h"

namespace biwa::nx {

// Switch formats are little-endian; structures are read with memcpy.
static_assert(std::endian::native == std::endian::little, "Biwa assumes a little-endian host");

// Read-only, random-access bytes: a file, a part of one, or a decrypting / decompressing layer
// over another storage (docs/design/03-extract.md, "Storage model"). Read is thread-safe.
class Storage {
   public:
    virtual ~Storage() = default;

    [[nodiscard]] virtual std::uint64_t Size() const = 0;

    // Fills `out` from `offset`. A range past Size() is Errc::kCorrupt.
    [[nodiscard]] virtual Status Read(std::uint64_t offset, std::span<std::byte> out) const = 0;
};

using StoragePtr = std::shared_ptr<const Storage>;

// True if [offset, offset + size) lies within [0, total), without overflow.
constexpr bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

// The Errc::kCorrupt error for a read that does not fit.
std::unexpected<Error> ReadPastEnd(std::uint64_t offset, std::uint64_t size, std::uint64_t total);

Result<StoragePtr> OpenFileStorage(const std::filesystem::path& path);

StoragePtr MakeMemoryStorage(std::vector<std::byte> data);

// The window [offset, offset + size) of `base`. Errc::kCorrupt, naming `what`, if it does not
// fit inside `base`.
Result<StoragePtr> MakeSubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size, std::string_view what);

// Reads a trivially copyable little-endian structure at `offset`.
template <class T>
    requires std::is_trivially_copyable_v<T>
Result<T> ReadStruct(const Storage& storage, std::uint64_t offset) {
    std::array<std::byte, sizeof(T)> bytes{};
    BIWA_RETURN_IF_ERROR(storage.Read(offset, bytes));
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

struct CopyOptions {
    unsigned threads = 0;                           // 0: one per hardware thread
    std::size_t piece_size = std::size_t{8} << 20;  // bytes per read / write
};

// Copies all of `from` to `to` starting at `to_offset`, in pieces over several threads. Stops at
// the first error and returns it.
Status CopyStorage(const Storage& from, OutputFile& to, std::uint64_t to_offset = 0, CopyOptions options = {});

}  // namespace biwa::nx
