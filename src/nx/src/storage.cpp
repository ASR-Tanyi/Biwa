// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/storage.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace biwa::nx {
namespace {

class FileStorage final : public Storage {
   public:
    explicit FileStorage(InputFile file) : file_(std::move(file)) {}

    [[nodiscard]] std::uint64_t Size() const override { return file_.Size(); }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        return file_.ReadAt(offset, out);
    }

   private:
    InputFile file_;
};

class MemoryStorage final : public Storage {
   public:
    explicit MemoryStorage(std::vector<std::byte> data) : data_(std::move(data)) {}

    [[nodiscard]] std::uint64_t Size() const override { return data_.size(); }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), data_.size())) {
            return ReadPastEnd(offset, out.size(), data_.size());
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return {};
    }

   private:
    std::vector<std::byte> data_;
};

class SubStorage final : public Storage {
   public:
    SubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size)
        : base_(std::move(base)), offset_(offset), size_(size) {}

    [[nodiscard]] std::uint64_t Size() const override { return size_; }

    [[nodiscard]] Status Read(std::uint64_t offset, std::span<std::byte> out) const override {
        if (!RangeFits(offset, out.size(), size_)) {
            return ReadPastEnd(offset, out.size(), size_);
        }
        return base_->Read(offset_ + offset, out);
    }

   private:
    StoragePtr base_;
    std::uint64_t offset_;
    std::uint64_t size_;
};

}  // namespace

std::unexpected<Error> ReadPastEnd(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return Failf(Errc::kCorrupt, "read of {:#x} bytes at {:#x} ends past the end (size {:#x})", size, offset, total);
}

Result<StoragePtr> OpenFileStorage(const std::filesystem::path& path) {
    Result<InputFile> file = InputFile::Open(path);
    if (!file) {
        return std::unexpected(std::move(file).error());
    }
    return std::make_shared<FileStorage>(std::move(*file));
}

StoragePtr MakeMemoryStorage(std::vector<std::byte> data) { return std::make_shared<MemoryStorage>(std::move(data)); }

Result<StoragePtr> MakeSubStorage(StoragePtr base, std::uint64_t offset, std::uint64_t size, std::string_view what) {
    if (!RangeFits(offset, size, base->Size())) {
        return Failf(Errc::kCorrupt, "{} ({:#x} bytes at {:#x}) does not fit in its container (size {:#x})", what, size,
                     offset, base->Size());
    }
    return std::make_shared<SubStorage>(std::move(base), offset, size);
}

Status ForEachPiece(const Storage& from, const PieceFn& fn, CopyOptions options) {
    const std::uint64_t size = from.Size();
    const std::size_t piece = std::max<std::size_t>(options.piece_size, 1);
    const std::uint64_t pieces = (size + piece - 1) / piece;
    unsigned threads = options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = static_cast<unsigned>(std::min<std::uint64_t>(threads, std::max<std::uint64_t>(pieces, 1)));

    std::atomic<std::uint64_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::optional<Error> first_error;

    auto worker = [&] {
        std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(piece, size)));
        while (!failed.load(std::memory_order_relaxed)) {
            const std::uint64_t index = next.fetch_add(1, std::memory_order_relaxed);
            if (index >= pieces) {
                return;
            }
            const std::uint64_t offset = index * piece;
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(piece, size - offset));
            const std::span<std::byte> chunk(buffer.data(), length);
            Status status = from.Read(offset, chunk);
            if (status) {
                status = fn(offset, chunk);
            }
            if (!status) {
                const std::scoped_lock lock(error_mutex);
                if (!first_error) {
                    first_error = std::move(status).error();
                }
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    std::vector<std::thread> pool;  // libc++ 18 has no std::jthread without -fexperimental-library
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    if (first_error) {
        return std::unexpected(std::move(*first_error));
    }
    return {};
}

Status CopyStorage(const Storage& from, OutputFile& to, std::uint64_t to_offset, CopyOptions options) {
    return ForEachPiece(
        from,
        [&to, to_offset](std::uint64_t offset, std::span<const std::byte> piece) {
            return to.WriteAt(to_offset + offset, piece);
        },
        options);
}

}  // namespace biwa::nx
