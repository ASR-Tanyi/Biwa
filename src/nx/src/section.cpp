// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/section.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "biwa/nx/compressed.h"
#include "biwa/nx/hash.h"

namespace biwa::nx {
namespace {

constexpr std::uint64_t kBlock = 16;

class AesCtrStorage final : public Storage {
   public:
    AesCtrStorage(StoragePtr base, const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t base_offset)
        : base_(std::move(base)), key_(key), counter_upper_(counter_upper), base_offset_(base_offset) {}

    [[nodiscard]] std::uint64_t Size() const override { return base_->Size(); }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), Size())) {
            return ReadPastEnd(offset, out.size(), Size());
        }
        return ReadAesCtr(*base_, key_, counter_upper_, base_offset_, offset, out);
    }

   private:
    StoragePtr base_;
    Aes128Key key_;
    std::uint64_t counter_upper_;
    std::uint64_t base_offset_;
};

template <class T>
T Field(const std::array<std::byte, 0xF8>& data, std::size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

// A section's hash tree: level 0 is checked against the master hash, each level after it against
// the level before, and the last level is the file system.
struct HashLevel {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t block_size = 0;
};
struct HashTree {
    std::vector<std::byte> master;
    std::vector<HashLevel> levels;
    HashPadding padding = HashPadding::kNone;
};

Result<HashTree> ReadHashTree(const NcaSection& section) {
    HashTree tree;
    const auto& data = section.hash_data;
    switch (section.hash_type) {
        case NcaHashType::kHierarchicalSha256: {
            // master hash (0x20), block size (u32), layer count (u32), regions [5] x {offset, size}.
            // The master hash covers all of region 0 at once.
            const auto block_size = Field<std::uint32_t>(data, 0x20);
            const auto layers = Field<std::uint32_t>(data, 0x24);
            if (layers < 1 || layers > 5) {
                return Failf(Errc::kCorrupt, "section {}: {} hash layers", section.index, layers);
            }
            tree.master.assign(data.begin(), data.begin() + 0x20);
            for (std::uint32_t i = 0; i < layers; ++i) {
                const HashLevel level{Field<std::uint64_t>(data, 0x28 + i * 0x10),
                                      Field<std::uint64_t>(data, 0x30 + i * 0x10), block_size};
                tree.levels.push_back(level);
            }
            tree.levels[0].block_size = tree.levels[0].size;
            tree.padding = HashPadding::kNone;
            return tree;
        }
        case NcaHashType::kHierarchicalIntegrity: {
            // "IVFC", version, master hash size (u32), max layers (u32),
            // levels [6] x {offset, size, block order (u32), reserved}, salt (0x20), master hash (0x20)
            if (Field<std::uint32_t>(data, 0) != 0x43465649) {  // "IVFC"
                return Failf(Errc::kCorrupt, "section {}: bad IVFC magic", section.index);
            }
            if (Field<std::uint32_t>(data, 0x08) != 0x20) {
                return Failf(Errc::kCorrupt, "section {}: IVFC master hash size is {:#x}", section.index,
                             Field<std::uint32_t>(data, 0x08));
            }
            const auto max_layers = Field<std::uint32_t>(data, 0x0C);
            if (max_layers < 2 || max_layers > 7) {
                return Failf(Errc::kCorrupt, "section {}: {} IVFC layers", section.index, max_layers);
            }
            tree.master.assign(data.begin() + 0xC0, data.begin() + 0xE0);
            for (std::uint32_t i = 0; i + 1 < max_layers; ++i) {
                const std::size_t at = 0x10 + std::size_t{i} * 0x18;
                const auto order = Field<std::uint32_t>(data, at + 0x10);
                if (order >= 32) {
                    return Failf(Errc::kCorrupt, "section {}: IVFC level {} block order {}", section.index, i, order);
                }
                tree.levels.push_back(
                    {Field<std::uint64_t>(data, at), Field<std::uint64_t>(data, at + 8), std::uint64_t{1} << order});
            }
            tree.padding = HashPadding::kZeros;
            return tree;
        }
        default:
            return Failf(Errc::kUnsupported, "section {} uses {} hashing, which is not supported yet", section.index,
                         HashTypeName(section.hash_type));
    }
}

Result<StoragePtr> OpenCheckedData(const StoragePtr& section_storage, const NcaSection& section) {
    Result<HashTree> tree = ReadHashTree(section);
    if (!tree) {
        return std::unexpected(std::move(tree).error());
    }
    StoragePtr hashes = MakeMemoryStorage(std::move(tree->master));
    for (std::size_t i = 0; i < tree->levels.size(); ++i) {
        const HashLevel& level = tree->levels[i];
        const bool last = i + 1 == tree->levels.size();
        std::string what = last ? std::format("section {} data", section.index)
                                : std::format("section {} hash level {}", section.index, i);
        Result<StoragePtr> raw = MakeSubStorage(section_storage, level.offset, level.size, what);
        if (!raw) {
            return raw;
        }
        Result<StoragePtr> checked =
            MakeHashCheckedStorage(hashes, std::move(*raw), level.block_size, tree->padding, std::move(what));
        if (!checked) {
            return checked;
        }
        if (i == 0) {
            // The top level is small (one block per master hash); keep it, checked, in memory.
            std::vector<std::byte> top(static_cast<std::size_t>(level.size));
            BIWA_RETURN_IF_ERROR((*checked)->Read(0, top));
            hashes = MakeMemoryStorage(std::move(top));
        } else {
            hashes = std::move(*checked);
        }
    }
    return hashes;
}

}  // namespace

Status ReadAesCtr(const Storage& raw, const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t base_offset,
                  std::uint64_t offset, std::span<std::byte> out) {
    if (out.empty()) {
        return {};
    }
    if (offset % kBlock == 0) {
        // The common case: decrypt in place.
        BIWA_RETURN_IF_ERROR(raw.Read(offset, out));
        AesCtrCrypt(key, NcaCounter(counter_upper, base_offset + offset), out);
        return {};
    }
    const std::uint64_t start = offset / kBlock * kBlock;
    const std::uint64_t end = std::min(raw.Size(), (offset + out.size() + kBlock - 1) / kBlock * kBlock);
    std::vector<std::byte> buffer(static_cast<std::size_t>(end - start));
    BIWA_RETURN_IF_ERROR(raw.Read(start, buffer));
    AesCtrCrypt(key, NcaCounter(counter_upper, base_offset + start), buffer);
    std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset - start), out.size(), out.begin());
    return {};
}

StoragePtr MakeAesCtrStorage(StoragePtr base, const Aes128Key& key, std::uint64_t counter_upper,
                             std::uint64_t base_offset) {
    return std::make_shared<AesCtrStorage>(std::move(base), key, counter_upper, base_offset);
}

Result<StoragePtr> OpenNcaSection(StoragePtr nca, const NcaSection& section, const std::optional<Aes128Key>& ctr_key) {
    if (section.HasPatch()) {
        return Failf(Errc::kUnsupported, "section {} is a patch (BKTR); patches are not supported yet", section.index);
    }
    if (section.HasSparse()) {
        return Failf(Errc::kUnsupported, "section {} is sparse; sparse sections are not supported yet", section.index);
    }
    Result<StoragePtr> raw =
        MakeSubStorage(std::move(nca), section.offset, section.size, std::format("NCA section {}", section.index));
    if (!raw) {
        return raw;
    }
    StoragePtr decrypted;
    switch (section.encryption) {
        case NcaEncryption::kNone:
            decrypted = std::move(*raw);
            break;
        case NcaEncryption::kAesCtr:
        case NcaEncryption::kAesCtrSkipLayerHash:
            if (!ctr_key) {
                return Failf(Errc::kInternal, "section {} is AES-CTR encrypted but no key was given", section.index);
            }
            decrypted = MakeAesCtrStorage(std::move(*raw), *ctr_key, section.counter_upper, section.offset);
            break;
        default:
            return Failf(Errc::kUnsupported, "section {} uses {} encryption, which is not supported yet", section.index,
                         EncryptionName(section.encryption));
    }
    return decrypted;
}

Result<SectionDataRegion> FindSectionData(const NcaSection& section, std::uint64_t storage_size) {
    SectionDataRegion region;
    switch (section.hash_type) {
        case NcaHashType::kNone:
            region = {0, storage_size};
            break;
        case NcaHashType::kHierarchicalSha256: {
            // master hash (0x20), block size (u32), layer count (u32), regions [5] x {offset, size}
            const auto layers = Field<std::uint32_t>(section.hash_data, 0x24);
            if (layers < 1 || layers > 5) {
                return Failf(Errc::kCorrupt, "section {}: {} hash layers", section.index, layers);
            }
            const std::size_t at = 0x28 + (layers - 1) * 0x10;
            region = {Field<std::uint64_t>(section.hash_data, at), Field<std::uint64_t>(section.hash_data, at + 8)};
            break;
        }
        case NcaHashType::kHierarchicalIntegrity: {
            // "IVFC", version, master hash size, max layers (u32), levels [6] x {offset, size, block order, pad}
            if (Field<std::uint32_t>(section.hash_data, 0) != 0x43465649) {  // "IVFC"
                return Failf(Errc::kCorrupt, "section {}: bad IVFC magic", section.index);
            }
            const auto max_layers = Field<std::uint32_t>(section.hash_data, 0x0C);
            if (max_layers < 2 || max_layers > 7) {
                return Failf(Errc::kCorrupt, "section {}: {} IVFC layers", section.index, max_layers);
            }
            const std::size_t at = 0x10 + (max_layers - 2) * 0x18;
            region = {Field<std::uint64_t>(section.hash_data, at), Field<std::uint64_t>(section.hash_data, at + 8)};
            break;
        }
        default:
            return Failf(Errc::kUnsupported, "section {} uses {} hashing, which is not supported yet", section.index,
                         HashTypeName(section.hash_type));
    }
    if (!RangeFits(region.offset, region.size, storage_size)) {
        return Failf(Errc::kCorrupt, "section {}: data ({:#x} bytes at {:#x}) is outside the section ({:#x})",
                     section.index, region.size, region.offset, storage_size);
    }
    return region;
}

namespace {

// The file system level of the section, as stored (compressed sections still compressed).
Result<StoragePtr> OpenStoredData(StoragePtr section_storage, const NcaSection& section, HashCheck check) {
    if (check == HashCheck::kOn && section.hash_type != NcaHashType::kNone) {
        return OpenCheckedData(section_storage, section);
    }
    const Result<SectionDataRegion> region = FindSectionData(section, section_storage->Size());
    if (!region) {
        return std::unexpected(region.error());
    }
    return MakeSubStorage(std::move(section_storage), region->offset, region->size,
                          std::format("section {} data", section.index));
}

}  // namespace

Result<StoragePtr> OpenSectionData(StoragePtr section_storage, const NcaSection& section, HashCheck check) {
    Result<StoragePtr> data = OpenStoredData(std::move(section_storage), section, check);
    if (!data || !section.HasCompression()) {
        return data;
    }
    // Compression sits above the hash layer: the hashes cover the stored (compressed) bytes.
    Result<StoragePtr> decompressed = MakeCompressedStorage(std::move(*data), section.compression_info);
    if (!decompressed) {
        return std::unexpected(std::move(decompressed).error().WithContext(std::format("section {}", section.index)));
    }
    return decompressed;
}

}  // namespace biwa::nx
