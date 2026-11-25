// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/pfs0.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace biwa::nx {
namespace {

constexpr std::uint32_t kMagic = 0x30534650;  // "PFS0"
constexpr std::uint64_t kHeaderSize = 0x10;
constexpr std::uint64_t kEntrySize = 0x18;
// Real containers hold a handful of files; this only stops a damaged count from asking for
// gigabytes before the bounds checks can fail.
constexpr std::uint64_t kMaxTableBytes = std::uint64_t{64} << 20;

struct Header {
    std::uint32_t magic;
    std::uint32_t file_count;
    std::uint32_t string_table_size;
    std::uint32_t reserved;
};
static_assert(sizeof(Header) == kHeaderSize);

struct RawEntry {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t name_offset;
    std::uint32_t reserved;
};
static_assert(sizeof(RawEntry) == kEntrySize);

}  // namespace

Result<Pfs0> Pfs0::Open(StoragePtr storage) {
    if (storage->Size() < kHeaderSize) {
        return Failf(Errc::kCorrupt, "too small for a PFS0 header ({} bytes)", storage->Size());
    }
    const Result<Header> header = ReadStruct<Header>(*storage, 0);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->magic != kMagic) {
        return Failf(Errc::kInvalidArgument, "not a PFS0 container (magic {:#010x})", header->magic);
    }

    const std::uint64_t entries_size = std::uint64_t{header->file_count} * kEntrySize;
    const std::uint64_t table_size = entries_size + header->string_table_size;
    if (table_size > kMaxTableBytes || !RangeFits(kHeaderSize, table_size, storage->Size())) {
        return Failf(Errc::kCorrupt, "PFS0 tables ({} files, {:#x}-byte string table) do not fit in {:#x} bytes",
                     header->file_count, header->string_table_size, storage->Size());
    }
    std::vector<std::byte> table(static_cast<std::size_t>(table_size));
    BIWA_RETURN_IF_ERROR(storage->Read(kHeaderSize, table));
    const std::uint64_t data_offset = kHeaderSize + table_size;
    const std::uint64_t data_size = storage->Size() - data_offset;
    const std::span<const std::byte> strings = std::span(table).subspan(static_cast<std::size_t>(entries_size));

    std::vector<Pfs0Entry> entries;
    entries.reserve(header->file_count);
    std::unordered_set<std::string_view> names;
    for (std::uint32_t i = 0; i < header->file_count; ++i) {
        RawEntry raw{};
        std::memcpy(&raw, table.data() + i * kEntrySize, sizeof(raw));
        if (raw.name_offset >= strings.size()) {
            return Failf(Errc::kCorrupt, "PFS0 entry {}: name offset {:#x} is outside the string table", i,
                         raw.name_offset);
        }
        const std::span<const std::byte> rest = strings.subspan(raw.name_offset);
        const auto end = std::ranges::find(rest, std::byte{0});
        if (end == rest.end()) {
            return Failf(Errc::kCorrupt, "PFS0 entry {}: name is not terminated", i);
        }
        std::string name(reinterpret_cast<const char*>(rest.data()), static_cast<std::size_t>(end - rest.begin()));
        if (name.empty()) {
            return Failf(Errc::kCorrupt, "PFS0 entry {}: empty name", i);
        }
        if (!RangeFits(raw.offset, raw.size, data_size)) {
            return Failf(Errc::kCorrupt, "PFS0 file {} ({:#x} bytes at {:#x}) ends past the container", name, raw.size,
                         raw.offset);
        }
        entries.push_back({std::move(name), data_offset + raw.offset, raw.size});
    }
    for (const Pfs0Entry& entry : entries) {
        if (!names.insert(entry.name).second) {
            return Failf(Errc::kCorrupt, "PFS0 has two files named {}", entry.name);
        }
    }
    return Pfs0(std::move(storage), std::move(entries));
}

const Pfs0Entry* Pfs0::Find(std::string_view name) const {
    const auto it = std::ranges::find(entries_, name, &Pfs0Entry::name);
    return it == entries_.end() ? nullptr : &*it;
}

Result<StoragePtr> Pfs0::OpenEntry(const Pfs0Entry& entry) const {
    return MakeSubStorage(storage_, entry.offset, entry.size, entry.name);
}

}  // namespace biwa::nx
