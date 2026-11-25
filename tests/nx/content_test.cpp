// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/content.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "biwa/core/hex.h"
#include "support/nca_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::KeySet;
using biwa::test::Bytes;

constexpr std::array<std::uint8_t, 16> kRightsId = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

KeySet Keys() {
    const std::string text = biwa::test::TestKeysText(biwa::test::TestKey(1), 0) +
                             "titlekek_00 = " + biwa::ToHex(biwa::test::TestKey(9)) + "\n";
    auto keys = KeySet::Parse(text, "test");
    EXPECT_TRUE(keys.has_value());
    return *keys;
}

biwa::nx::Pfs0 Package(bool with_ticket) {
    std::vector<biwa::test::BuildFile> files = {{"broken.tik", Bytes("not a ticket")}};
    if (with_ticket) {
        files.push_back({"good.tik", biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(5), biwa::test::TestKey(9))});
    }
    auto pfs = biwa::nx::Pfs0::Open(biwa::nx::MakeMemoryStorage(biwa::test::BuildPfs0(files)));
    EXPECT_TRUE(pfs.has_value());
    return std::move(*pfs);
}

TEST(ContentTest, ReadsTicketsAndReportsBadOnes) {
    std::vector<std::string> bad;
    const biwa::nx::TicketMap tickets =
        biwa::nx::ReadTickets(Package(true), [&](std::string_view name, const biwa::Error&) { bad.emplace_back(name); });
    EXPECT_EQ(bad, std::vector<std::string>{"broken.tik"});
    ASSERT_EQ(tickets.size(), 1u);
    EXPECT_TRUE(tickets.contains(biwa::ToHex(kRightsId)));
}

TEST(ContentTest, OpensASectionWithItsTitleKey) {
    const std::vector<std::byte> payload = Bytes("file system bytes");
    const biwa::test::HashedSection hashed = biwa::test::BuildIvfcSection(payload);
    biwa::test::NcaBuildSection section;
    section.fs_type = biwa::nx::NcaFsType::kRomFs;
    section.hash_type = biwa::nx::NcaHashType::kHierarchicalIntegrity;
    section.encryption = biwa::nx::NcaEncryption::kAesCtr;
    section.hash_data = hashed.hash_data;
    section.data = hashed.data;
    biwa::test::NcaBuildOptions options;
    options.rights_id = kRightsId;
    options.ctr_key = biwa::test::TestKey(5);
    const biwa::nx::StoragePtr nca = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(options, {section}));
    const auto header = biwa::nx::ReadNcaHeader(*nca, Keys());
    ASSERT_TRUE(header.has_value()) << header.error();

    const biwa::nx::TicketMap tickets = biwa::nx::ReadTickets(Package(true), [](auto, const auto&) {});
    const auto fs = biwa::nx::OpenSectionFs(nca, *header, header->sections[0], Keys(), tickets);
    ASSERT_TRUE(fs.has_value()) << fs.error();
    std::vector<std::byte> out(payload.size());
    ASSERT_TRUE((*fs)->Read(0, out).has_value());
    EXPECT_EQ(out, payload);

    const auto missing = biwa::nx::OpenSectionFs(nca, *header, header->sections[0], Keys(), {});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), Errc::kMissingKey);
}

}  // namespace
