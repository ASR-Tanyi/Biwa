// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/hash.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include "biwa/core/sha256.h"

namespace biwa::nx {
namespace {

constexpr std::uint64_t kHashSize = 32;

class HashCheckedStorage final : public Storage {
   public:
    HashCheckedStorage(StoragePtr hashes, StoragePtr data, std::uint64_t block_size, HashPadding padding,
                       std::string what)
        : hashes_(std::move(hashes)),
          data_(std::move(data)),
          block_size_(block_size),
          padding_(padding),
          what_(std::move(what)) {}

    [[nodiscard]] std::uint64_t Size() const override { return data_->Size(); }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        const std::uint64_t size = Size();
        if (!RangeFits(offset, out.size(), size)) {
            return ReadPastEnd(offset, out.size(), size);
        }
        if (out.empty()) {
            return {};
        }
        const std::uint64_t first = offset / block_size_;
        const std::uint64_t end = (offset + out.size() - 1) / block_size_ + 1;
        const std::uint64_t start = first * block_size_;
        const std::uint64_t stop = std::min(size, end * block_size_);

        std::vector<std::byte> expected(static_cast<std::size_t>((end - first) * kHashSize));
        BIWA_RETURN_IF_ERROR(hashes_->Read(first * kHashSize, expected));

        // Whole blocks go straight into `out`; otherwise read the blocks around the range.
        std::vector<std::byte> buffer;
        std::span<std::byte> blocks = out;
        if (start != offset || stop != offset + out.size()) {
            buffer.resize(static_cast<std::size_t>(stop - start));
            blocks = buffer;
        }
        BIWA_RETURN_IF_ERROR(data_->Read(start, blocks));

        Sha256 sha;
        for (std::uint64_t block = first; block < end; ++block) {
            const auto at = static_cast<std::size_t>((block - first) * block_size_);
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(block_size_, blocks.size() - at));
            sha.Update(blocks.subspan(at, length));
            if (padding_ == HashPadding::kZeros) {
                UpdateZeros(sha, block_size_ - length);
            }
            const Sha256Digest digest = sha.Finish();
            if (std::memcmp(digest.data(), expected.data() + (block - first) * kHashSize, kHashSize) != 0) {
                return Failf(Errc::kCorrupt, "{}: block {} (bytes {:#x} to {:#x}) does not match its hash", what_,
                             block, block * block_size_, block * block_size_ + length);
            }
        }
        if (!buffer.empty()) {
            std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset - start), out.size(), out.begin());
        }
        return {};
    }

   private:
    static void UpdateZeros(Sha256& sha, std::uint64_t count) {
        static constexpr std::array<std::byte, 4096> kZeros{};
        while (count != 0) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(count, kZeros.size()));
            sha.Update(std::span(kZeros).first(n));
            count -= n;
        }
    }

    StoragePtr hashes_;
    StoragePtr data_;
    std::uint64_t block_size_;
    HashPadding padding_;
    std::string what_;
};

}  // namespace

Result<StoragePtr> MakeHashCheckedStorage(StoragePtr hashes, StoragePtr data, std::uint64_t block_size,
                                          HashPadding padding, std::string what) {
    if (block_size == 0 || block_size > kMaxHashBlockSize) {
        return Failf(Errc::kCorrupt, "{}: hash block size {:#x} is out of range", what, block_size);
    }
    const std::uint64_t blocks = data->Size() / block_size + (data->Size() % block_size != 0 ? 1 : 0);
    if (hashes->Size() / kHashSize < blocks) {
        return Failf(Errc::kCorrupt, "{}: {} blocks but only {} hashes", what, blocks, hashes->Size() / kHashSize);
    }
    return std::make_shared<HashCheckedStorage>(std::move(hashes), std::move(data), block_size, padding,
                                                std::move(what));
}

}  // namespace biwa::nx
