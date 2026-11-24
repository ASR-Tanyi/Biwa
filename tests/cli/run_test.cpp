// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "biwa/core/version.h"

namespace {

struct Result {
    int code;
    std::string out;
    std::string err;
};

Result RunBiwa(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    std::ostringstream out;
    std::ostringstream err;
    const int code = biwa::cli::Run(list, out, err);
    return {code, out.str(), err.str()};
}

TEST(CliTest, NoArgumentsPrintsUsage) {
    const Result r = RunBiwa({});
    EXPECT_EQ(r.code, biwa::cli::kExitUsage);
    EXPECT_TRUE(r.out.empty());
    EXPECT_TRUE(r.err.starts_with("usage: biwa")) << r.err;
}

TEST(CliTest, VersionPrintsVersionAndCommit) {
    for (const std::string_view command : {"version", "--version"}) {
        const Result r = RunBiwa({command});
        EXPECT_EQ(r.code, biwa::cli::kExitOk);
        const std::string expected =
            "biwa " + std::string(biwa::Version()) + " (" + std::string(biwa::SourceCommit()) + ")\n";
        EXPECT_EQ(r.out, expected);
        EXPECT_TRUE(r.err.empty());
    }
}

TEST(CliTest, HelpPrintsUsageToOut) {
    for (const std::string_view command : {"help", "--help", "-h"}) {
        const Result r = RunBiwa({command});
        EXPECT_EQ(r.code, biwa::cli::kExitOk);
        EXPECT_TRUE(r.out.starts_with("usage: biwa")) << r.out;
        EXPECT_TRUE(r.err.empty());
    }
}

TEST(CliTest, UnknownCommandIsAUsageError) {
    const Result r = RunBiwa({"frobnicate"});
    EXPECT_EQ(r.code, biwa::cli::kExitUsage);
    EXPECT_TRUE(r.out.empty());
    EXPECT_TRUE(r.err.starts_with("biwa: unknown command 'frobnicate'")) << r.err;
}

TEST(CliTest, ExtraArgumentIsAUsageError) {
    const Result r = RunBiwa({"version", "now"});
    EXPECT_EQ(r.code, biwa::cli::kExitUsage);
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(r.err, "biwa version: unexpected argument 'now'\n");
}

}  // namespace
