// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/patch.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "biwa/nx/section.h"

namespace biwa::nx {
namespace {

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

class AesCtrExStorage final : public Storage {
   public:
    AesCtrExStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t base_offset,
                    BucketTree tree)
        : raw_(std::move(raw)),
          key_(key),
          counter_upper_(counter_upper),
          base_offset_(base_offset),
          tree_(std::move(tree)) {}

    [[nodiscard]] std::uint64_t Size() const override { return raw_->Size(); }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), Size())) {
            return ReadPastEnd(offset, out.size(), Size());
        }
        std::size_t done = 0;
        while (done < out.size()) {
            const std::uint64_t position = offset + done;
            if (tree_.count() == 0 || position >= tree_.end_offset) {
                // The tables after the counted range use the section's own counter.
                return ReadAesCtr(*raw_, key_, counter_upper_, base_offset_, position, out.subspan(done));
            }
            const std::size_t index = tree_.Find(position);
            if (index == tree_.count()) {
                return Failf(Errc::kCorrupt, "patch section: no counter for offset {:#x}", position);
            }
            const std::span<const std::byte> entry = tree_.Entry(index);
            const auto length =
                static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, tree_.End(index) - position));
            const std::span<std::byte> piece = out.subspan(done, length);
            switch (Load<std::uint8_t>(entry, 0x08)) {
                case 0: {  // encrypted
                    const auto generation = Load<std::uint32_t>(entry, 0x0C);
                    const std::uint64_t upper = (counter_upper_ & 0xFFFFFFFF00000000) | generation;
                    BIWA_RETURN_IF_ERROR(ReadAesCtr(*raw_, key_, upper, base_offset_, position, piece));
                    break;
                }
                case 1:  // stored as it is
                    BIWA_RETURN_IF_ERROR(raw_->Read(position, piece));
                    break;
                default:
                    return Failf(Errc::kCorrupt, "patch section: bad encryption value at {:#x}", tree_.Offset(index));
            }
            done += length;
        }
        return {};
    }

   private:
    StoragePtr raw_;
    Aes128Key key_;
    std::uint64_t counter_upper_;
    std::uint64_t base_offset_;
    BucketTree tree_;
};

class IndirectStorage final : public Storage {
   public:
    IndirectStorage(StoragePtr base, StoragePtr patch, BucketTree tree)
        : sources_{std::move(base), std::move(patch)}, tree_(std::move(tree)) {}

    [[nodiscard]] std::uint64_t Size() const override { return tree_.end_offset; }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), Size())) {
            return ReadPastEnd(offset, out.size(), Size());
        }
        std::size_t index = out.empty() ? 0 : tree_.Find(offset);
        for (std::size_t done = 0; done < out.size(); ++index) {
            const std::uint64_t position = offset + done;
            const std::span<const std::byte> entry = tree_.Entry(index);
            const auto length =
                static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, tree_.End(index) - position));
            const auto physical = Load<std::uint64_t>(entry, 0x08) + (position - tree_.Offset(index));
            const StoragePtr& source = sources_[Load<std::uint32_t>(entry, 0x10)];
            BIWA_RETURN_IF_ERROR(source->Read(physical, out.subspan(done, length)));
            done += length;
        }
        return {};
    }

   private:
    std::array<StoragePtr, 2> sources_;
    BucketTree tree_;
};

}  // namespace

StoragePtr MakeAesCtrExStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper,
                               std::uint64_t base_offset, BucketTree tree) {
    return std::make_shared<AesCtrExStorage>(std::move(raw), key, counter_upper, base_offset, std::move(tree));
}

Result<StoragePtr> MakeIndirectStorage(StoragePtr base, StoragePtr patch, BucketTree tree) {
    if (tree.count() == 0 || tree.Offset(0) != 0) {
        return Fail(Errc::kCorrupt, "patch section: the relocation table does not start at 0");
    }
    const std::array<const Storage*, 2> sources = {base.get(), patch.get()};
    for (std::size_t i = 0; i < tree.count(); ++i) {
        const std::span<const std::byte> entry = tree.Entry(i);
        const auto physical = Load<std::int64_t>(entry, 0x08);
        const auto index = Load<std::int32_t>(entry, 0x10);
        if (index < 0 || index > 1 || physical < 0 ||
            !RangeFits(static_cast<std::uint64_t>(physical), tree.End(i) - tree.Offset(i),
                       sources[static_cast<std::size_t>(index)]->Size())) {
            return Failf(Errc::kCorrupt, "patch section: relocation entry {} (at {:#x}) does not fit", i,
                         tree.Offset(i));
        }
    }
    return std::make_shared<IndirectStorage>(std::move(base), std::move(patch), std::move(tree));
}

Result<StoragePtr> OpenPatchedSection(StoragePtr base_section, StoragePtr patch_nca, const NcaSection& patch_section,
                                      const Aes128Key& key) {
    const auto what = std::format("section {}", patch_section.index);
    if (patch_section.encryption != NcaEncryption::kAesCtrEx) {
        return Failf(Errc::kUnsupported, "{}: a patch with {} encryption is not supported", what,
                     EncryptionName(patch_section.encryption));
    }
    if (patch_section.HasSparse()) {
        return Failf(Errc::kUnsupported, "{} is sparse; sparse sections are not supported yet", what);
    }
    const std::span<const std::byte> info(patch_section.patch_info);
    const Result<BucketTreeInfo> relocation = ParseBucketTreeInfo(info.first(0x20), what + " relocation table");
    if (!relocation) {
        return std::unexpected(relocation.error());
    }
    const Result<BucketTreeInfo> counters = ParseBucketTreeInfo(info.subspan(0x20, 0x20), what + " counter table");
    if (!counters) {
        return std::unexpected(counters.error());
    }
    Result<StoragePtr> raw =
        MakeSubStorage(std::move(patch_nca), patch_section.offset, patch_section.size, "NCA " + what);
    if (!raw) {
        return raw;
    }
    // The counter table itself is plain AES-CTR with the section's counter.
    const StoragePtr plain = MakeAesCtrStorage(*raw, key, patch_section.counter_upper, patch_section.offset);
    Result<BucketTree> counter_tree = ReadBucketTree(*plain, *counters, kAesCtrExEntrySize, what + " counter table");
    if (!counter_tree) {
        return std::unexpected(std::move(counter_tree).error());
    }
    const StoragePtr decrypted = MakeAesCtrExStorage(std::move(*raw), key, patch_section.counter_upper,
                                                     patch_section.offset, std::move(*counter_tree));
    Result<BucketTree> relocation_tree =
        ReadBucketTree(*decrypted, *relocation, kIndirectEntrySize, what + " relocation table");
    if (!relocation_tree) {
        return std::unexpected(std::move(relocation_tree).error());
    }
    return MakeIndirectStorage(std::move(base_section), decrypted, std::move(*relocation_tree));
}

}  // namespace biwa::nx
