// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/ticket.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

#include "biwa/core/hex.h"
#include "support/nca_builder.h"

namespace {

using biwa::Errc;
using biwa::nx::KeySet;

constexpr std::array<std::uint8_t, 16> kRightsId = {0x01, 0x00, 0x8C, 0xF0, 0x1B, 0xAA, 0xC0, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12};

std::string HexKey(const biwa::nx::Aes128Key& key) { return biwa::ToHex(key); }

KeySet Keys(unsigned revision) {
    const std::string text = biwa::test::TestKeysText(biwa::test::TestKey(1), revision) +
                             std::format("titlekek_{:02x} = {}\n", revision, HexKey(biwa::test::TestKey(9)));
    auto keys = KeySet::Parse(text, "test");
    EXPECT_TRUE(keys.has_value());
    return *keys;
}

biwa::Result<biwa::nx::Ticket> Read(std::vector<std::byte> bytes) {
    return biwa::nx::ReadTicket(*biwa::nx::MakeMemoryStorage(std::move(bytes)));
}

TEST(TicketTest, ReadsACommonTicket) {
    const auto ticket =
        Read(biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(5), biwa::test::TestKey(9), 0, 0x11));
    ASSERT_TRUE(ticket.has_value()) << ticket.error();
    EXPECT_EQ(ticket->rights_id, kRightsId);
    EXPECT_EQ(ticket->master_key_revision, 0x11);
    const auto title_key = biwa::nx::DecryptTitleKey(*ticket, Keys(0x11), 0x11);
    ASSERT_TRUE(title_key.has_value()) << title_key.error();
    EXPECT_EQ(*title_key, biwa::test::TestKey(5));
    EXPECT_EQ(biwa::nx::DecryptTitleKey(*ticket, Keys(0x11), 0x12).error().code(), Errc::kMissingKey);
}

TEST(TicketTest, PersonalisedTicketsAreUnsupported) {
    const auto ticket = Read(biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(5), biwa::test::TestKey(9), 1));
    ASSERT_FALSE(ticket.has_value());
    EXPECT_EQ(ticket.error().code(), Errc::kUnsupported);
    EXPECT_NE(ticket.error().message().find("01008cf01baac0000000000000000012"), std::string::npos);
}

TEST(TicketTest, BadTicketsAreCorrupt) {
    std::vector<std::byte> bytes = biwa::test::BuildTicket(kRightsId, {}, {});
    biwa::test::PutU32(bytes, 0, 0x12345);
    EXPECT_EQ(Read(bytes).error().code(), Errc::kCorrupt);
    bytes = biwa::test::BuildTicket(kRightsId, {}, {});
    bytes.resize(0x200);
    EXPECT_EQ(Read(bytes).error().code(), Errc::kCorrupt);
}

TEST(NcaCtrKeyTest, KeyAreaWithoutRightsId) {
    biwa::test::NcaBuildOptions options;
    options.key_generation = 3;  // revision 2
    options.key_area_key = biwa::test::TestKey(1);
    options.key_area[2] = biwa::test::TestKey(7);
    const auto storage = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(options, {}));
    const auto header = biwa::nx::ReadNcaHeader(*storage, Keys(2));
    ASSERT_TRUE(header.has_value()) << header.error();
    const auto key = biwa::nx::NcaCtrKey(*header, Keys(2), nullptr);
    ASSERT_TRUE(key.has_value()) << key.error();
    EXPECT_EQ(*key, biwa::test::TestKey(7));

    const auto missing = biwa::nx::NcaCtrKey(*header, Keys(3), nullptr);  // only key_area_key_..._03
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message(), "key_area_key_application_02 is not in the key files");
}

TEST(NcaCtrKeyTest, TitleKeyWithRightsId) {
    biwa::test::NcaBuildOptions options;
    options.rights_id = kRightsId;
    const auto storage = biwa::nx::MakeMemoryStorage(biwa::test::BuildNca(options, {}));
    const auto header = biwa::nx::ReadNcaHeader(*storage, Keys(0));
    ASSERT_TRUE(header.has_value());
    ASSERT_TRUE(header->HasRightsId());

    EXPECT_EQ(biwa::nx::NcaCtrKey(*header, Keys(0), nullptr).error().code(), Errc::kMissingKey);
    const auto ticket = Read(biwa::test::BuildTicket(kRightsId, biwa::test::TestKey(5), biwa::test::TestKey(9)));
    ASSERT_TRUE(ticket.has_value());
    const auto key = biwa::nx::NcaCtrKey(*header, Keys(0), &*ticket);
    ASSERT_TRUE(key.has_value()) << key.error();
    EXPECT_EQ(*key, biwa::test::TestKey(5));

    auto other = *ticket;
    other.rights_id[15] ^= 1;
    EXPECT_EQ(biwa::nx::NcaCtrKey(*header, Keys(0), &other).error().code(), Errc::kInternal);
}

}  // namespace
