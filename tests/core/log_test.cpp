// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/log.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace log = biwa::log;

struct Line {
    log::Level level;
    std::string subsystem;
    std::string message;
};

class CaptureSink final : public log::Sink {
   public:
    void Write(log::Level level, std::string_view subsystem, std::string_view message) override {
        lines.push_back({level, std::string(subsystem), std::string(message)});
    }
    std::vector<Line> lines;
};

// Each test logs into its own capture sink and restores the defaults afterwards.
class LogTest : public ::testing::Test {
   protected:
    void SetUp() override {
        sink_ = std::make_shared<CaptureSink>();
        log::SetSinks({sink_});
        log::SetLevel(log::Level::kInfo);
    }
    void TearDown() override {
        log::SetSinks({log::StderrSink()});
        log::SetLevel(log::Level::kInfo);
    }
    std::shared_ptr<CaptureSink> sink_;
};

TEST_F(LogTest, FormatsAndRoutesALine) {
    log::Info("fs", "mounted {} ({} files)", "romfs", 12);
    ASSERT_EQ(sink_->lines.size(), 1u);
    EXPECT_EQ(sink_->lines[0].level, log::Level::kInfo);
    EXPECT_EQ(sink_->lines[0].subsystem, "fs");
    EXPECT_EQ(sink_->lines[0].message, "mounted romfs (12 files)");
}

TEST_F(LogTest, DropsLinesBelowTheLevel) {
    log::Debug("svc", "hidden");
    log::Trace("svc", "hidden");
    log::Warn("svc", "shown");
    log::SetLevel(log::Level::kError);
    log::Warn("svc", "hidden");
    log::Err("svc", "shown");
    ASSERT_EQ(sink_->lines.size(), 2u);
    EXPECT_EQ(sink_->lines[0].level, log::Level::kWarn);
    EXPECT_EQ(sink_->lines[1].level, log::Level::kError);
    EXPECT_FALSE(log::Enabled(log::Level::kWarn));
    EXPECT_TRUE(log::Enabled(log::Level::kError));
}

TEST_F(LogTest, TraceLevelLetsEverythingThrough) {
    log::SetLevel(log::Level::kTrace);
    log::Trace("cpu", "a");
    log::Debug("cpu", "b");
    EXPECT_EQ(sink_->lines.size(), 2u);
}

TEST_F(LogTest, LinesFromManyThreadsStayWhole) {
    constexpr int kThreads = 8;
    constexpr int kLines = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) {
                log::Info("thread", "{}:{}", t, i);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sink_->lines.size(), static_cast<std::size_t>(kThreads * kLines));
}

TEST_F(LogTest, FileSinkWritesFormattedLines) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "biwa_log_test.txt";
    {
        std::shared_ptr<log::Sink> file = log::FileSink(path);
        ASSERT_NE(file, nullptr);
        log::SetSinks({file});
        log::Warn("gpu", "missing extension {}", "VK_EXT_example");
        log::SetSinks({});
    }
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    std::filesystem::remove(path);
    EXPECT_TRUE(text.str().starts_with('[')) << text.str();
    EXPECT_TRUE(text.str().ends_with("] warn  gpu: missing extension VK_EXT_example\n")) << text.str();
}

TEST_F(LogTest, FileSinkFailsForAMissingDirectory) {
    EXPECT_EQ(log::FileSink(std::filesystem::temp_directory_path() / "biwa-no-such-dir" / "x.txt"), nullptr);
}

TEST(LogLevelTest, NamesEveryLevel) {
    EXPECT_EQ(log::LevelName(log::Level::kTrace), "trace");
    EXPECT_EQ(log::LevelName(log::Level::kDebug), "debug");
    EXPECT_EQ(log::LevelName(log::Level::kInfo), "info");
    EXPECT_EQ(log::LevelName(log::Level::kWarn), "warn");
    EXPECT_EQ(log::LevelName(log::Level::kError), "error");
}

}  // namespace
