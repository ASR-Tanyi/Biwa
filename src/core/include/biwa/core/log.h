// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace biwa::log {

enum class Level : std::uint8_t { kTrace, kDebug, kInfo, kWarn, kError };

// "trace", "debug", "info", "warn", "error".
std::string_view LevelName(Level level);

// A destination for log lines. Write is called for one complete line at a time, under the
// logger's lock, so a sink needs no locking of its own and lines never interleave.
class Sink {
   public:
    virtual ~Sink() = default;
    virtual void Write(Level level, std::string_view subsystem, std::string_view message) = 0;
};

// Writes "[   12.345] info  fs: message" lines (seconds since the first log line) to stderr.
std::shared_ptr<Sink> StderrSink();

// Writes the same lines to a file, replacing it. Returns nullptr if the file cannot be created.
std::shared_ptr<Sink> FileSink(const std::filesystem::path& path);

// Replaces the sinks. The initial set is one StderrSink.
void SetSinks(std::vector<std::shared_ptr<Sink>> sinks);

// Lines below this level are dropped before they are formatted. The default is kInfo.
void SetLevel(Level level);
Level GetLevel();
bool Enabled(Level level);

// Sends one line to every sink. `subsystem` names the part of Biwa: "fs", "svc", "nvn", ...
void Write(Level level, std::string_view subsystem, std::string_view message);

template <class... Args>
void Logf(Level level, std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    if (Enabled(level)) {
        Write(level, subsystem, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void Trace(std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    Logf(Level::kTrace, subsystem, format, std::forward<Args>(args)...);
}
template <class... Args>
void Debug(std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    Logf(Level::kDebug, subsystem, format, std::forward<Args>(args)...);
}
template <class... Args>
void Info(std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    Logf(Level::kInfo, subsystem, format, std::forward<Args>(args)...);
}
template <class... Args>
void Warn(std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    Logf(Level::kWarn, subsystem, format, std::forward<Args>(args)...);
}
template <class... Args>
void Err(std::string_view subsystem, std::format_string<Args...> format, Args&&... args) {
    Logf(Level::kError, subsystem, format, std::forward<Args>(args)...);
}

}  // namespace biwa::log
