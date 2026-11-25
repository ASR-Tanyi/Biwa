// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/cli/run.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "biwa/core/file.h"
#include "biwa/core/hex.h"
#include "biwa/core/version.h"
#include "support/nca_builder.h"
#include "support/pfs0_builder.h"
#include "support/romfs_builder.h"
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

// An NSP holding a ticket and a program NCA (rights ID, encrypted ExeFS), plus matching keys.
struct TestNsp {
    biwa::test::TempDir dir;
    std::string nsp;
    std::string keys;
};

void MakeTestNsp(TestNsp& t, bool with_ticket, bool damaged = false) {
    constexpr std::array<std::uint8_t, 16> kRightsId = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05};
    const std::vector<std::byte> exefs_files =
        biwa::test::BuildPfs0({{"main", biwa::test::Bytes("code")}, {"main.npdm", biwa::test::Bytes("meta")}});
    biwa::test::NcaBuildSection exefs;
    exefs.hash_type = biwa::nx::NcaHashType::kHierarchicalSha256;
    exefs.encryption = biwa::nx::NcaEncryption::kAesCtr;
    const biwa::test::HashedSection hashed = biwa::test::BuildSha256Section(exefs_files);
    exefs.hash_data = hashed.hash_data;
    exefs.data = hashed.data;
    biwa::test::NcaBuildOptions options;
    options.rights_id = kRightsId;
    options.ctr_key = biwa::test::TestKey(5);  // the title key
    std::vector<std::byte> nca = biwa::test::BuildNca(options, {exefs});
    if (damaged) {
        nca[biwa::nx::kNcaHeaderSize + hashed.payload_offset + 1] ^= std::byte{0x01};
    }
    std::vector<biwa::test::BuildFile> files = {{"00112233445566778899aabbccddeeff.nca", nca}};
    if (with_ticket) {
        files.push_back({"01000000000010000000000000000005.tik",
                         biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(5), biwa::test::TestKey(9))});
    }
    ASSERT_TRUE(biwa::WriteFileAtomic(t.dir / "game.nsp", biwa::test::BuildPfs0(files)).has_value());
    const std::string keys = biwa::test::TestKeysText(biwa::test::TestKey(1), 0) +
                             "titlekek_00 = " + biwa::ToHex(biwa::test::TestKey(9)) + "\n";
    ASSERT_TRUE(biwa::WriteFileAtomic(t.dir / "prod.keys", std::as_bytes(std::span(keys))).has_value());
    t.nsp = PathText(t.dir / "game.nsp");
    t.keys = PathText(t.dir / "prod.keys");
}

TEST(CliInfoTest, WithKeysDescribesNcasAndListsTheExefs) {
    TestNsp t;
    MakeTestNsp(t, true);
    const Result r = RunBiwa({"info", "-k", t.keys, t.nsp});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.out << r.err;
    EXPECT_NE(
        r.out.find("  00112233445566778899aabbccddeeff.nca\n"
                   "    program, title 0100000000001000, key generation 0, rights ID 01000000000010000000000000000005\n"
                   "    section 0: PFS0, sha256, aes-ctr, 1024 bytes\n"
                   "                   4  main\n"
                   "                   4  main.npdm\n"),
        std::string::npos)
        << r.out;
}

TEST(CliInfoTest, MissingTicketIsReportedAndExitsWithMissingKeys) {
    TestNsp t;
    MakeTestNsp(t, false);
    const Result r = RunBiwa({"info", "--keys", t.keys, t.nsp});
    EXPECT_EQ(r.code, biwa::cli::kExitMissingKeys) << r.out;
    EXPECT_NE(r.out.find("error: missing key: no ticket for rights ID 01000000000010000000000000000005"),
              std::string::npos)
        << r.out;
    EXPECT_EQ(RunBiwa({"info", "-k"}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"info", "-k", PathText(t.dir / "nope.keys"), t.nsp}).code, biwa::cli::kExitInput);
}

TEST(CliInfoTest, VerifyChecksEveryHash) {
    TestNsp good;
    MakeTestNsp(good, true);
    Result r = RunBiwa({"info", "-k", good.keys, "--verify", good.nsp});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.out << r.err;
    EXPECT_NE(r.out.find("                   4  main.npdm\n      hashes match\n"), std::string::npos) << r.out;

    TestNsp bad;
    MakeTestNsp(bad, true, true);
    r = RunBiwa({"info", "-k", bad.keys, "--verify", bad.nsp});
    EXPECT_EQ(r.code, biwa::cli::kExitDamaged) << r.out;
    const std::string error = "      error: corrupt: section 0 data: block 0 (bytes 0x0 to 0x68) does not match its hash\n";
    EXPECT_NE(r.out.find(error), std::string::npos) << r.out;
    EXPECT_EQ(r.out.find(error), r.out.rfind(error)) << "reported once\n" << r.out;

    r = RunBiwa({"info", "--verify", good.nsp});
    EXPECT_EQ(r.code, biwa::cli::kExitUsage);
    EXPECT_NE(r.err.find("--verify needs -k"), std::string::npos) << r.err;
}

std::vector<std::byte> SampleRomfs() {
    return biwa::test::BuildRomfs({
        {"Pack/Bootup.pack", biwa::test::Bytes("SARC bootup")},
        {"System/Version.txt", biwa::test::Bytes("1.0.0\n")},
        {"top.bin", biwa::test::Bytes("top")},
    });
}

struct RomfsImage {
    biwa::test::TempDir dir;
    std::string path;
    RomfsImage() : path(PathText(dir / "romfs.bin")) {
        EXPECT_TRUE(biwa::WriteFileAtomic(dir / "romfs.bin", SampleRomfs()).has_value());
    }
};

TEST(CliRomfsTest, LsListsADirectory) {
    const RomfsImage t;
    Result r = RunBiwa({"romfs", "ls", t.path});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_EQ(r.out,
              "                Pack/\n"
              "                System/\n"
              "             3  top.bin\n");
    r = RunBiwa({"romfs", "ls", t.path, "/System"});
    EXPECT_EQ(r.out, "             6  Version.txt\n");
}

TEST(CliRomfsTest, LsRecursiveListsFullPaths) {
    const RomfsImage t;
    Result r = RunBiwa({"romfs", "ls", "-r", t.path});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_EQ(r.out,
              "             3  /top.bin\n"
              "            11  /Pack/Bootup.pack\n"
              "             6  /System/Version.txt\n"
              "            20  bytes in 3 files\n");
    r = RunBiwa({"romfs", "ls", t.path, "Pack/", "-r"});
    EXPECT_EQ(r.out,
              "            11  /Pack/Bootup.pack\n"
              "            11  bytes in 1 file\n");
}

TEST(CliRomfsTest, CatPrintsOrWritesAFile) {
    const RomfsImage t;
    Result r = RunBiwa({"romfs", "cat", t.path, "System/Version.txt"});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_EQ(r.out, "1.0.0\n");
    const std::string copy = PathText(t.dir / "bootup.pack");
    r = RunBiwa({"romfs", "cat", "-o", copy, t.path, "/Pack/Bootup.pack"});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_TRUE(r.out.empty());
    EXPECT_EQ(biwa::ReadFileText(t.dir / "bootup.pack").value_or(""), "SARC bootup");
}

TEST(CliRomfsTest, ProblemsHaveExitCodes) {
    const RomfsImage t;
    EXPECT_EQ(RunBiwa({"romfs"}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"romfs", "rm", t.path}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"romfs", "cat", t.path}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"romfs", "ls", t.path, "a", "b"}).code, biwa::cli::kExitUsage);
    EXPECT_EQ(RunBiwa({"romfs", "ls", "-k"}).code, biwa::cli::kExitUsage);

    Result r = RunBiwa({"romfs", "cat", t.path, "Pack/Missing.pack"});
    EXPECT_EQ(r.code, biwa::cli::kExitInput);
    EXPECT_EQ(r.err, "biwa romfs: the RomFS has no Pack/Missing.pack\n");
    EXPECT_EQ(RunBiwa({"romfs", "ls", t.path, "top.bin"}).code, biwa::cli::kExitInput);

    ASSERT_TRUE(biwa::WriteFileAtomic(t.dir / "other.bin", biwa::test::Bytes("something else")).has_value());
    r = RunBiwa({"romfs", "ls", PathText(t.dir / "other.bin")});
    EXPECT_EQ(r.code, biwa::cli::kExitInput);
    EXPECT_NE(r.err.find("is neither an NSP nor a RomFS image"), std::string::npos) << r.err;

    ASSERT_TRUE(biwa::WriteFileAtomic(t.dir / "empty.nsp", biwa::test::BuildPfs0({})).has_value());
    r = RunBiwa({"romfs", "ls", PathText(t.dir / "empty.nsp")});
    EXPECT_EQ(r.code, biwa::cli::kExitMissingKeys);
    EXPECT_NE(r.err.find("an NSP needs -k <prod.keys>"), std::string::npos) << r.err;
}

TEST(CliRomfsTest, ReadsTheRomfsOfAnNsp) {
    constexpr std::array<std::uint8_t, 16> kRightsId = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};
    const biwa::test::HashedSection hashed = biwa::test::BuildIvfcSection(SampleRomfs());
    biwa::test::NcaBuildSection romfs;
    romfs.fs_type = biwa::nx::NcaFsType::kRomFs;
    romfs.hash_type = biwa::nx::NcaHashType::kHierarchicalIntegrity;
    romfs.encryption = biwa::nx::NcaEncryption::kAesCtr;
    romfs.counter_upper = 0x0000000100000000;
    romfs.hash_data = hashed.hash_data;
    romfs.data = hashed.data;
    biwa::test::NcaBuildOptions options;
    options.rights_id = kRightsId;
    options.ctr_key = biwa::test::TestKey(6);
    biwa::test::NcaBuildOptions control;
    control.content_type = biwa::nx::NcaContentType::kControl;  // skipped: not program data

    const biwa::test::TempDir dir;
    const std::vector<biwa::test::BuildFile> files = {
        {"aa.nca", biwa::test::BuildNca(control, {romfs})},
        {"bb.nca", biwa::test::BuildNca(options, {romfs})},
        {"01000000000030000000000000000007.tik",
         biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(6), biwa::test::TestKey(9))},
    };
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "game.nsp", biwa::test::BuildPfs0(files)).has_value());
    const std::string keys = biwa::test::TestKeysText(biwa::test::TestKey(1), 0) +
                             "titlekek_00 = " + biwa::ToHex(biwa::test::TestKey(9)) + "\n";
    ASSERT_TRUE(biwa::WriteFileAtomic(dir / "prod.keys", std::as_bytes(std::span(keys))).has_value());

    const Result r =
        RunBiwa({"romfs", "cat", "-k", PathText(dir / "prod.keys"), PathText(dir / "game.nsp"), "System/Version.txt"});
    EXPECT_EQ(r.code, biwa::cli::kExitOk) << r.err;
    EXPECT_EQ(r.out, "1.0.0\n");
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
