// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "biwa/core/error.h"

namespace biwa {

// A file opened for reading. ReadAt is thread-safe: any number of threads may read the same
// InputFile at once (pread on POSIX, ReadFile at an offset on Windows).
class InputFile {
   public:
    // Errc::kNotFound if the file does not exist, Errc::kIo for other failures.
    static Result<InputFile> Open(const std::filesystem::path& path);

    InputFile(InputFile&& other) noexcept;
    InputFile& operator=(InputFile&& other) noexcept;
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    ~InputFile();

    // The size when the file was opened.
    [[nodiscard]] std::uint64_t Size() const { return size_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    // Fills `out` from `offset`. A range past the end of the file is Errc::kCorrupt.
    [[nodiscard]] Status ReadAt(std::uint64_t offset, std::span<std::byte> out) const;

   private:
    InputFile(std::filesystem::path path, std::intptr_t handle, std::uint64_t size);

    std::filesystem::path path_;
    std::intptr_t handle_;  // file descriptor (POSIX) or HANDLE (Windows); -1 when moved from
    std::uint64_t size_;
};

// A file opened for writing: created, or emptied if it exists. WriteAt is thread-safe for
// ranges that do not overlap. Close reports errors; the destructor closes silently.
class OutputFile {
   public:
    static Result<OutputFile> Create(const std::filesystem::path& path);

    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile();

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] Status WriteAt(std::uint64_t offset, std::span<const std::byte> data);
    // Extends with zeros or truncates.
    [[nodiscard]] Status SetSize(std::uint64_t size);
    [[nodiscard]] Status Close();

   private:
    OutputFile(std::filesystem::path path, std::intptr_t handle);

    std::filesystem::path path_;
    std::intptr_t handle_;
};

// Whole-file helpers for small files (keys, manifests, profiles).
Result<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path);
Result<std::string> ReadFileText(const std::filesystem::path& path);

// Writes `<path>.tmp`, then renames it over `path`, so readers see the old file or the new one,
// never a partial one.
Status WriteFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data);

}  // namespace biwa
