// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/section.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>
#include <vector>

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
        if (out.empty()) {
            return {};
        }
        if (offset % kBlock == 0) {
            // The common case: decrypt in place.
            BIWA_RETURN_IF_ERROR(base_->Read(offset, out));
            AesCtrCrypt(key_, NcaCounter(counter_upper_, base_offset_ + offset), out);
            return {};
        }
        const std::uint64_t start = offset / kBlock * kBlock;
        const std::uint64_t end = std::min(Size(), (offset + out.size() + kBlock - 1) / kBlock * kBlock);
        std::vector<std::byte> buffer(static_cast<std::size_t>(end - start));
        BIWA_RETURN_IF_ERROR(base_->Read(start, buffer));
        AesCtrCrypt(key_, NcaCounter(counter_upper_, base_offset_ + start), buffer);
        std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset - start), out.size(), out.begin());
        return {};
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

}  // namespace

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
    if (section.HasCompression()) {
        return Failf(Errc::kUnsupported, "section {} is compressed; compressed sections are not supported yet",
                     section.index);
    }
    Result<StoragePtr> raw =
        MakeSubStorage(std::move(nca), section.offset, section.size, std::format("NCA section {}", section.index));
    if (!raw) {
        return raw;
    }
    switch (section.encryption) {
        case NcaEncryption::kNone:
            return raw;
        case NcaEncryption::kAesCtr:
        case NcaEncryption::kAesCtrSkipLayerHash:
            if (!ctr_key) {
                return Failf(Errc::kInternal, "section {} is AES-CTR encrypted but no key was given", section.index);
            }
            return MakeAesCtrStorage(std::move(*raw), *ctr_key, section.counter_upper, section.offset);
        default:
            return Failf(Errc::kUnsupported, "section {} uses {} encryption, which is not supported yet", section.index,
                         EncryptionName(section.encryption));
    }
}

Result<SectionDataRegion> FindSectionData(const NcaSection& section) {
    SectionDataRegion region;
    switch (section.hash_type) {
        case NcaHashType::kNone:
            region = {0, section.size};
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
    if (!RangeFits(region.offset, region.size, section.size)) {
        return Failf(Errc::kCorrupt, "section {}: data ({:#x} bytes at {:#x}) is outside the section ({:#x})",
                     section.index, region.size, region.offset, section.size);
    }
    return region;
}

Result<StoragePtr> OpenSectionData(StoragePtr section_storage, const NcaSection& section) {
    const Result<SectionDataRegion> region = FindSectionData(section);
    if (!region) {
        return std::unexpected(region.error());
    }
    return MakeSubStorage(std::move(section_storage), region->offset, region->size,
                          std::format("section {} data", section.index));
}

}  // namespace biwa::nx
