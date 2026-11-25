// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "biwa/core/file.h"
#include "biwa/core/version.h"
#include "support/pfs0_builder.h"
#include "support/temp_dir.h"

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
    EXPECT_EQ(r.err, "biwa version: unexpected argument 'now'\nusage: biwa version\n");
}

std::string PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

TEST(CliInfoTest, ListsTheFilesOfAnNsp) {
    const biwa::test::TempDir dir;
    const auto nsp = biwa::test::BuildPfs0({{"0123456789abcdef0123456789abcdef.nca", biwa::test::Bytes("12345")},
                                            {"0123456789abcdef0123456789abcdef.tik", biwa::test::Bytes("xy")}});
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "game.nsp", nsp).has_value());
    const std::string path = PathText(dir / "game.nsp");
    const Result r = RunBiwa({"info", path});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_EQ(r.out, path +
                         "\n  PFS0, 2 files\n"
                         "               5  0123456789abcdef0123456789abcdef.nca\n"
                         "               2  0123456789abcdef0123456789abcdef.tik\n"
                         "               7  bytes in all\n");
    EXPECT_TRUE(r.err.empty());
}

TEST(CliInfoTest, MapsErrorsToExitCodes) {
    const biwa::test::TempDir dir;
    const Result missing = RunBiwa({"info", PathText(dir / "none.nsp")});
    EXPECT_EQ(missing.code, biwa::cli::kExitInput);
    EXPECT_TRUE(missing.err.starts_with("biwa info: cannot open ")) << missing.err;

    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "text.txt", biwa::test::Bytes("hello, not a container")).has_value());
    const Result wrong = RunBiwa({"info", PathText(dir / "text.txt")});
    EXPECT_EQ(wrong.code, biwa::cli::kExitInput);
    EXPECT_NE(wrong.err.find("not a PFS0 container"), std::string::npos) << wrong.err;

    auto damaged = biwa::test::BuildPfs0({{"a", biwa::test::Bytes("abc")}});
    biwa::test::PutU64(damaged, 0x10 + 8, 100);
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "damaged.nsp", damaged).has_value());
    const Result bad = RunBiwa({"info", PathText(dir / "damaged.nsp")});
    EXPECT_EQ(bad.code, biwa::cli::kExitDamaged);

    EXPECT_EQ(RunBiwa({"info"}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"info", "a", "b"}).code, biwa::cli::kExitUsage);
}

TEST(CliTest, ExitCodesCoverEveryErrorCategory) {
    using biwa::Errc;
    using biwa::cli::ExitCodeFor;
    EXPECT_EQ(ExitCodeFor(Errc::kInvalidArgument), 2);
    EXPECT_EQ(ExitCodeFor(Errc::kNotFound), 2);
    EXPECT_EQ(ExitCodeFor(Errc::kIo), 2);
    EXPECT_EQ(ExitCodeFor(Errc::kCorrupt), 3);
    EXPECT_EQ(ExitCodeFor(Errc::kMissingKey), 4);
    EXPECT_EQ(ExitCodeFor(Errc::kUnsupported), 5);
    EXPECT_EQ(ExitCodeFor(Errc::kInternal), 70);
}

}  // namespace
