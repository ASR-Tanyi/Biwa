// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/romfs.h"

#include <array>
#include <cstring>
#include <format>

namespace biwa::nx {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFF;
constexpr std::uint64_t kHeaderSize = 0x50;
constexpr std::uint64_t kMaxTableSize = std::uint64_t{256} << 20;
constexpr std::size_t kDirEntrySize = 0x18;
constexpr std::size_t kFileEntrySize = 0x20;
constexpr std::uint32_t kMaxNameSize = 0x300;

template <class T>
T Load(const std::vector<std::byte>& bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

Status CheckName(std::string_view name, std::string_view kind, std::uint32_t offset) {
    const bool bad = name.empty() || name == "." || name == ".." || name.find_first_of(std::string_view("/\\\0", 3)) !=
                                                                         std::string_view::npos;
    if (bad) {
        return Failf(Errc::kCorrupt, "RomFS: {} entry {:#x} has an invalid name", kind, offset);
    }
    return {};
}

std::unexpected<Error> Loop() { return Fail(Errc::kCorrupt, "RomFS: the directory tables loop"); }

}  // namespace

struct Romfs::Dir {
    std::uint32_t sibling = kNone;
    std::uint32_t child_dir = kNone;
    std::uint32_t child_file = kNone;
    std::string_view name;
};

struct Romfs::File {
    std::uint32_t sibling = kNone;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::string_view name;
};

struct Romfs::Found {
    bool is_directory = true;
    std::uint32_t offset = 0;
};

Result<Romfs> Romfs::Open(StoragePtr storage) {
    // header size, then {offset, size} of: directory hash table, directory table, file hash table,
    // file table; then the offset of the file data.
    std::array<std::uint64_t, 10> header{};
    if (storage->Size() < kHeaderSize) {
        return Failf(Errc::kCorrupt, "RomFS: {} bytes is too small for a header", storage->Size());
    }
    BIWA_RETURN_IF_ERROR(storage->Read(0, std::as_writable_bytes(std::span(header))));
    if (header[0] != kHeaderSize) {
        return Failf(Errc::kCorrupt, "RomFS: header size is {:#x}, not {:#x}", header[0], kHeaderSize);
    }
    auto table = [&](std::uint64_t offset, std::uint64_t size, std::string_view what) -> Result<std::vector<std::byte>> {
        if (size > kMaxTableSize || !RangeFits(offset, size, storage->Size())) {
            return Failf(Errc::kCorrupt, "RomFS: the {} table ({:#x} bytes at {:#x}) does not fit", what, size,
                         offset);
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        BIWA_RETURN_IF_ERROR(storage->Read(offset, bytes));
        return bytes;
    };
    Result<std::vector<std::byte>> dirs = table(header[3], header[4], "directory");
    if (!dirs) {
        return std::unexpected(std::move(dirs).error());
    }
    Result<std::vector<std::byte>> files = table(header[7], header[8], "file");
    if (!files) {
        return std::unexpected(std::move(files).error());
    }
    if (header[9] > storage->Size()) {
        return Failf(Errc::kCorrupt, "RomFS: file data starts at {:#x}, past the end", header[9]);
    }
    Romfs romfs(std::move(storage), std::move(*dirs), std::move(*files), header[9]);
    const Result<Dir> root = romfs.ReadDir(0);
    if (!root) {
        return std::unexpected(root.error());
    }
    return romfs;
}

Result<Romfs::Dir> Romfs::ReadDir(std::uint32_t offset) const {
    if (offset % 4 != 0 || !RangeFits(offset, kDirEntrySize, dirs_.size())) {
        return Failf(Errc::kCorrupt, "RomFS: directory entry {:#x} is outside the table", offset);
    }
    // parent, sibling, first child directory, first file, next in hash bucket, name size, name
    const auto name_size = Load<std::uint32_t>(dirs_, offset + 0x14);
    if (name_size > kMaxNameSize || !RangeFits(offset + kDirEntrySize, name_size, dirs_.size())) {
        return Failf(Errc::kCorrupt, "RomFS: directory entry {:#x} has a bad name size", offset);
    }
    return Dir{Load<std::uint32_t>(dirs_, offset + 0x04), Load<std::uint32_t>(dirs_, offset + 0x08),
               Load<std::uint32_t>(dirs_, offset + 0x0C),
               std::string_view(reinterpret_cast<const char*>(dirs_.data() + offset + kDirEntrySize), name_size)};
}

Result<Romfs::File> Romfs::ReadFile(std::uint32_t offset) const {
    if (offset % 4 != 0 || !RangeFits(offset, kFileEntrySize, files_.size())) {
        return Failf(Errc::kCorrupt, "RomFS: file entry {:#x} is outside the table", offset);
    }
    // parent, sibling, data offset (u64), data size (u64), next in hash bucket, name size, name
    const auto name_size = Load<std::uint32_t>(files_, offset + 0x1C);
    if (name_size > kMaxNameSize || !RangeFits(offset + kFileEntrySize, name_size, files_.size())) {
        return Failf(Errc::kCorrupt, "RomFS: file entry {:#x} has a bad name size", offset);
    }
    return File{Load<std::uint32_t>(files_, offset + 0x04), Load<std::uint64_t>(files_, offset + 0x08),
                Load<std::uint64_t>(files_, offset + 0x10),
                std::string_view(reinterpret_cast<const char*>(files_.data() + offset + kFileEntrySize), name_size)};
}

std::size_t Romfs::MaxSteps() const { return dirs_.size() / kDirEntrySize + files_.size() / kFileEntrySize + 1; }

Result<Romfs::Found> Romfs::Find(std::string_view path) const {
    Found current;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = std::min(path.find('/', at), path.size());
        const std::string_view name = path.substr(at, slash - at);
        at = slash + 1;
        if (name.empty()) {
            continue;
        }
        if (!current.is_directory) {
            return Failf(Errc::kNotFound, "the RomFS has no {}", path);
        }
        const Result<Dir> dir = ReadDir(current.offset);
        if (!dir) {
            return std::unexpected(dir.error());
        }
        std::size_t budget = MaxSteps();
        bool found = false;
        for (std::uint32_t child = dir->child_dir; child != kNone && !found;) {
            const Result<Dir> entry = ReadDir(child);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            if (--budget == 0) {
                return Loop();
            }
            if (entry->name == name) {
                current = {true, child};
                found = true;
            }
            child = entry->sibling;
        }
        for (std::uint32_t file = dir->child_file; file != kNone && !found;) {
            const Result<File> entry = ReadFile(file);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            if (--budget == 0) {
                return Loop();
            }
            if (entry->name == name) {
                current = {false, file};
                found = true;
            }
            file = entry->sibling;
        }
        if (!found) {
            return Failf(Errc::kNotFound, "the RomFS has no {}", path);
        }
    }
    return current;
}

Result<std::vector<RomfsEntry>> Romfs::List(std::string_view path) const {
    const Result<Found> found = Find(path);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!found->is_directory) {
        return Failf(Errc::kInvalidArgument, "{} is a file in the RomFS, not a directory", path);
    }
    const Result<Dir> dir = ReadDir(found->offset);
    if (!dir) {
        return std::unexpected(dir.error());
    }
    std::vector<RomfsEntry> entries;
    std::size_t budget = MaxSteps();
    for (std::uint32_t child = dir->child_dir; child != kNone;) {
        const Result<Dir> entry = ReadDir(child);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        BIWA_RETURN_IF_ERROR(CheckName(entry->name, "directory", child));
        if (--budget == 0) {
            return Loop();
        }
        entries.push_back({std::string(entry->name), true, 0});
        child = entry->sibling;
    }
    for (std::uint32_t file = dir->child_file; file != kNone;) {
        const Result<File> entry = ReadFile(file);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        BIWA_RETURN_IF_ERROR(CheckName(entry->name, "file", file));
        if (--budget == 0) {
            return Loop();
        }
        entries.push_back({std::string(entry->name), false, entry->size});
        file = entry->sibling;
    }
    return entries;
}

Result<StoragePtr> Romfs::OpenFile(std::string_view path) const {
    const Result<Found> found = Find(path);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (found->is_directory) {
        return Failf(Errc::kInvalidArgument, "{} is a directory in the RomFS, not a file", path);
    }
    const Result<File> file = ReadFile(found->offset);
    if (!file) {
        return std::unexpected(file.error());
    }
    if (!RangeFits(file->offset, file->size, storage_->Size() - data_offset_)) {
        return Failf(Errc::kCorrupt, "RomFS: the data of {} ({:#x} bytes at {:#x}) is past the end", path,
                     file->size, file->offset);
    }
    return MakeSubStorage(storage_, data_offset_ + file->offset, file->size, path);
}

Status Romfs::ForEachFile(const FileFn& fn) const {
    struct Pending {
        std::uint32_t dir = 0;
        std::string path;
    };
    std::vector<Pending> stack = {{0, ""}};
    std::size_t budget = MaxSteps();
    while (!stack.empty()) {
        const Pending pending = std::move(stack.back());
        stack.pop_back();
        const Result<Dir> dir = ReadDir(pending.dir);
        if (!dir) {
            return std::unexpected(dir.error());
        }
        for (std::uint32_t file = dir->child_file; file != kNone;) {
            const Result<File> entry = ReadFile(file);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            BIWA_RETURN_IF_ERROR(CheckName(entry->name, "file", file));
            if (--budget == 0) {
                return Loop();
            }
            BIWA_RETURN_IF_ERROR(fn(std::format("{}/{}", pending.path, entry->name), entry->size));
            file = entry->sibling;
        }
        std::vector<Pending> children;
        for (std::uint32_t child = dir->child_dir; child != kNone;) {
            const Result<Dir> entry = ReadDir(child);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            BIWA_RETURN_IF_ERROR(CheckName(entry->name, "directory", child));
            if (--budget == 0) {
                return Loop();
            }
            children.push_back({child, std::format("{}/{}", pending.path, entry->name)});
            child = entry->sibling;
        }
        stack.insert(stack.end(), std::make_move_iterator(children.rbegin()), std::make_move_iterator(children.rend()));
    }
    return {};
}

}  // namespace biwa::nx
