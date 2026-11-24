// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/file.h"

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace biwa {
namespace {

constexpr std::intptr_t kNoHandle = -1;

std::string PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

#ifdef _WIN32

HANDLE AsHandle(std::intptr_t handle) { return reinterpret_cast<HANDLE>(handle); }  // NOLINT(performance-no-int-to-ptr)
std::intptr_t FromHandle(HANDLE handle) { return reinterpret_cast<std::intptr_t>(handle); }

std::string SystemMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        text.resize(static_cast<std::size_t>(std::max(bytes, 0)));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), bytes, nullptr, nullptr);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
            text.pop_back();
        }
    }
    LocalFree(buffer);
    return std::format("{} (error {})", text.empty() ? "unknown error" : text, code);
}

std::unexpected<Error> SystemFailure(std::string_view what, const std::filesystem::path& path) {
    const DWORD code = GetLastError();
    const Errc errc = (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? Errc::kNotFound : Errc::kIo;
    return Failf(errc, "{} {}: {}", what, PathText(path), SystemMessage(code));
}

OVERLAPPED OverlappedAt(std::uint64_t offset) {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

constexpr std::size_t kMaxChunk = 1u << 30;  // ReadFile / WriteFile take a 32-bit size

#else

std::unexpected<Error> SystemFailure(std::string_view what, const std::filesystem::path& path) {
    const int code = errno;
    const Errc errc = (code == ENOENT || code == ENOTDIR) ? Errc::kNotFound : Errc::kIo;
    return Failf(errc, "{} {}: {} (errno {})", what, PathText(path), std::generic_category().message(code), code);
}

constexpr std::size_t kMaxChunk = 1u << 30;

#endif

void CloseHandleIfOpen(std::intptr_t handle) {
    if (handle == kNoHandle) {
        return;
    }
#ifdef _WIN32
    CloseHandle(AsHandle(handle));
#else
    close(static_cast<int>(handle));
#endif
}

}  // namespace

// ---- InputFile ----

InputFile::InputFile(std::filesystem::path path, std::intptr_t handle, std::uint64_t size)
    : path_(std::move(path)), handle_(handle), size_(size) {}

InputFile::InputFile(InputFile&& other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, kNoHandle)), size_(other.size_) {}

InputFile& InputFile::operator=(InputFile&& other) noexcept {
    if (this != &other) {
        CloseHandleIfOpen(handle_);
        path_ = std::move(other.path_);
        handle_ = std::exchange(other.handle_, kNoHandle);
        size_ = other.size_;
    }
    return *this;
}

InputFile::~InputFile() { CloseHandleIfOpen(handle_); }

Result<InputFile> InputFile::Open(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return SystemFailure("cannot open", path);
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0) {
        auto failure = SystemFailure("cannot get the size of", path);
        CloseHandle(handle);
        return failure;
    }
    return InputFile(path, FromHandle(handle), static_cast<std::uint64_t>(size.QuadPart));
#else
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return SystemFailure("cannot open", path);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0) {
        auto failure = SystemFailure("cannot get the size of", path);
        close(fd);
        return failure;
    }
    if (!S_ISREG(info.st_mode)) {
        close(fd);
        return Failf(Errc::kInvalidArgument, "{} is not a regular file", PathText(path));
    }
    return InputFile(path, fd, static_cast<std::uint64_t>(info.st_size));
#endif
}

Status InputFile::ReadAt(std::uint64_t offset, std::span<std::byte> out) const {
    if (offset > size_ || out.size() > size_ - offset) {
        return Failf(Errc::kCorrupt, "{}: read of {:#x} bytes at {:#x} ends past the end of the file (size {:#x})",
                     PathText(path_), out.size(), offset, size_);
    }
    while (!out.empty()) {
        const std::size_t chunk = std::min(out.size(), kMaxChunk);
#ifdef _WIN32
        OVERLAPPED overlapped = OverlappedAt(offset);
        DWORD done = 0;
        if (ReadFile(AsHandle(handle_), out.data(), static_cast<DWORD>(chunk), &done, &overlapped) == 0) {
            return SystemFailure("cannot read", path_);
        }
        const std::size_t got = done;
#else
        const ssize_t n = pread(static_cast<int>(handle_), out.data(), chunk, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SystemFailure("cannot read", path_);
        }
        const auto got = static_cast<std::size_t>(n);
#endif
        if (got == 0) {
            return Failf(Errc::kCorrupt, "{}: the file became shorter while it was read", PathText(path_));
        }
        out = out.subspan(got);
        offset += got;
    }
    return {};
}

// ---- OutputFile ----

OutputFile::OutputFile(std::filesystem::path path, std::intptr_t handle) : path_(std::move(path)), handle_(handle) {}

OutputFile::OutputFile(OutputFile&& other) noexcept
    : path_(std::move(other.path_)), handle_(std::exchange(other.handle_, kNoHandle)) {}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept {
    if (this != &other) {
        CloseHandleIfOpen(handle_);
        path_ = std::move(other.path_);
        handle_ = std::exchange(other.handle_, kNoHandle);
    }
    return *this;
}

OutputFile::~OutputFile() { CloseHandleIfOpen(handle_); }

Result<OutputFile> OutputFile::Create(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return SystemFailure("cannot create", path);
    }
    return OutputFile(path, FromHandle(handle));
#else
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return SystemFailure("cannot create", path);
    }
    return OutputFile(path, fd);
#endif
}

Status OutputFile::WriteAt(std::uint64_t offset, std::span<const std::byte> data) {
    if (handle_ == kNoHandle) {
        return Failf(Errc::kInternal, "{}: write after close", PathText(path_));
    }
    while (!data.empty()) {
        const std::size_t chunk = std::min(data.size(), kMaxChunk);
#ifdef _WIN32
        OVERLAPPED overlapped = OverlappedAt(offset);
        DWORD done = 0;
        if (WriteFile(AsHandle(handle_), data.data(), static_cast<DWORD>(chunk), &done, &overlapped) == 0) {
            return SystemFailure("cannot write", path_);
        }
        const std::size_t put = done;
#else
        const ssize_t n = pwrite(static_cast<int>(handle_), data.data(), chunk, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SystemFailure("cannot write", path_);
        }
        const auto put = static_cast<std::size_t>(n);
#endif
        if (put == 0) {
            return Failf(Errc::kIo, "cannot write {}: no progress", PathText(path_));
        }
        data = data.subspan(put);
        offset += put;
    }
    return {};
}

Status OutputFile::SetSize(std::uint64_t size) {
    if (handle_ == kNoHandle) {
        return Failf(Errc::kInternal, "{}: resize after close", PathText(path_));
    }
#ifdef _WIN32
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (SetFileInformationByHandle(AsHandle(handle_), FileEndOfFileInfo, &info, sizeof(info)) == 0) {
        return SystemFailure("cannot resize", path_);
    }
#else
    if (ftruncate(static_cast<int>(handle_), static_cast<off_t>(size)) != 0) {
        return SystemFailure("cannot resize", path_);
    }
#endif
    return {};
}

Status OutputFile::Close() {
    const std::intptr_t handle = std::exchange(handle_, kNoHandle);
    if (handle == kNoHandle) {
        return {};
    }
#ifdef _WIN32
    if (CloseHandle(AsHandle(handle)) == 0) {
        return SystemFailure("cannot close", path_);
    }
#else
    if (close(static_cast<int>(handle)) != 0) {
        return SystemFailure("cannot close", path_);
    }
#endif
    return {};
}

// ---- Whole files ----

Result<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path) {
    Result<InputFile> file = InputFile::Open(path);
    if (!file) {
        return std::unexpected(std::move(file).error());
    }
    if (file->Size() > std::numeric_limits<std::size_t>::max()) {
        return Failf(Errc::kUnsupported, "{} is too large to read at once", PathText(path));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file->Size()));
    BIWA_RETURN_IF_ERROR(file->ReadAt(0, bytes));
    return bytes;
}

Result<std::string> ReadFileText(const std::filesystem::path& path) {
    Result<std::vector<std::byte>> bytes = ReadFileBytes(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    std::string text(bytes->size(), '\0');
    std::ranges::transform(*bytes, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

Status WriteFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        Result<OutputFile> file = OutputFile::Create(temp);
        if (!file) {
            return std::unexpected(std::move(file).error());
        }
        BIWA_RETURN_IF_ERROR(file->WriteAt(0, data));
        BIWA_RETURN_IF_ERROR(file->Close());
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        std::filesystem::remove(temp, ec);
        return Failf(Errc::kIo, "cannot replace {}: {}", PathText(path), reason);
    }
    return {};
}

}  // namespace biwa
