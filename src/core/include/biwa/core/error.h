// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace biwa {

// What went wrong. The categories map onto the biwa command's exit codes
// (docs/design/03-extract.md), so a command can report any error it receives.
enum class Errc : std::uint8_t {
    kInvalidArgument,  // the caller passed something invalid
    kNotFound,         // a file, key or entry does not exist
    kIo,               // the operating system failed or refused
    kCorrupt,          // damaged data: bad magic, hash mismatch, truncated, out of range
    kMissingKey,       // a key needed to decrypt is not in the key files
    kUnsupported,      // valid input that Biwa does not handle (yet)
    kInternal,         // a bug in Biwa
};

// "invalid argument", "not found", "i/o", "corrupt", "missing key", "unsupported", "internal".
std::string_view ErrcName(Errc code);

// An error: a category and a message for people. Messages never contain key material.
class Error {
   public:
    Error(Errc code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] Errc code() const { return code_; }
    [[nodiscard]] const std::string& message() const { return message_; }

    // Prefixes the message with what was being done: "reading header: file is truncated".
    [[nodiscard]] Error WithContext(std::string_view context) && {
        message_.insert(0, std::string(context) + ": ");
        return std::move(*this);
    }

    friend bool operator==(const Error&, const Error&) = default;

   private:
    Errc code_;
    std::string message_;
};

// "corrupt: bad magic". For logs and test failures.
std::ostream& operator<<(std::ostream& os, const Error& error);

template <class T>
using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;

// return Fail(Errc::kCorrupt, "bad magic");
inline std::unexpected<Error> Fail(Errc code, std::string message) {
    return std::unexpected<Error>(std::in_place, code, std::move(message));
}

// return Failf(Errc::kCorrupt, "section {} ends past the file ({:#x} > {:#x})", index, end, size);
template <class... Args>
std::unexpected<Error> Failf(Errc code, std::format_string<Args...> format, Args&&... args) {
    return Fail(code, std::format(format, std::forward<Args>(args)...));
}

}  // namespace biwa

// Returns the error of a Result or Status from the enclosing function, which must itself
// return a Result or Status. The expression is evaluated once.
#define BIWA_RETURN_IF_ERROR(expr)                                                  \
    do {                                                                            \
        if (auto biwa_result_ = (expr); !biwa_result_.has_value()) {                \
            return std::unexpected<::biwa::Error>(std::move(biwa_result_).error()); \
        }                                                                           \
    } while (false)
