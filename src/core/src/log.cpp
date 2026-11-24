// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace biwa::log {
namespace {

using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    std::vector<std::shared_ptr<Sink>> sinks{StderrSink()};
    std::atomic<Level> level{Level::kInfo};
    Clock::time_point start = Clock::now();
};

State& GetState() {
    static State state;  // constructed on first use, so logging works during static initialisation
    return state;
}

std::string FormatLine(Level level, std::string_view subsystem, std::string_view message) {
    const std::chrono::duration<double> since = Clock::now() - GetState().start;
    return std::format("[{:>10.3f}] {:<5} {}: {}\n", since.count(), LevelName(level), subsystem, message);
}

class StderrSinkImpl final : public Sink {
   public:
    void Write(Level level, std::string_view subsystem, std::string_view message) override {
        const std::string line = FormatLine(level, subsystem, message);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
};

class FileSinkImpl final : public Sink {
   public:
    explicit FileSinkImpl(std::ofstream file) : file_(std::move(file)) {}

    void Write(Level level, std::string_view subsystem, std::string_view message) override {
        file_ << FormatLine(level, subsystem, message);
        file_.flush();
    }

   private:
    std::ofstream file_;
};

}  // namespace

std::string_view LevelName(Level level) {
    switch (level) {
        case Level::kTrace:
            return "trace";
        case Level::kDebug:
            return "debug";
        case Level::kInfo:
            return "info";
        case Level::kWarn:
            return "warn";
        case Level::kError:
            return "error";
    }
    return "?";
}

std::shared_ptr<Sink> StderrSink() { return std::make_shared<StderrSinkImpl>(); }

std::shared_ptr<Sink> FileSink(const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!file) {
        return nullptr;
    }
    return std::make_shared<FileSinkImpl>(std::move(file));
}

void SetSinks(std::vector<std::shared_ptr<Sink>> sinks) {
    State& state = GetState();
    const std::scoped_lock lock(state.mutex);
    state.sinks = std::move(sinks);
}

void SetLevel(Level level) { GetState().level.store(level, std::memory_order_relaxed); }

Level GetLevel() { return GetState().level.load(std::memory_order_relaxed); }

bool Enabled(Level level) { return level >= GetLevel(); }

void Write(Level level, std::string_view subsystem, std::string_view message) {
    if (!Enabled(level)) {
        return;
    }
    State& state = GetState();
    const std::scoped_lock lock(state.mutex);
    for (const std::shared_ptr<Sink>& sink : state.sinks) {
        if (sink) {
            sink->Write(level, subsystem, message);
        }
    }
}

}  // namespace biwa::log
