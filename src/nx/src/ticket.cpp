// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/ticket.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "biwa/core/hex.h"

namespace biwa::nx {
namespace {

// Bytes from the start of the ticket to its data, by signature type.
Result<std::uint64_t> DataOffset(std::uint32_t signature_type) {
    switch (signature_type) {
        case 0x010000:  // RSA-4096, SHA-1
        case 0x010003:  // RSA-4096, SHA-256
            return 4 + 0x200 + 0x3C;
        case 0x010001:  // RSA-2048, SHA-1
        case 0x010004:  // RSA-2048, SHA-256
            return 4 + 0x100 + 0x3C;
        case 0x010002:  // ECDSA, SHA-1
        case 0x010005:  // ECDSA, SHA-256
            return 4 + 0x3C + 0x40;
        case 0x010006:  // HMAC-SHA1
            return 4 + 0x14 + 0x28;
        default:
            return Failf(Errc::kCorrupt, "unknown ticket signature type {:#x}", signature_type);
    }
}

constexpr std::uint64_t kDataSize = 0x180;

std::string_view KeyAreaName(NcaKeyArea area) {
    switch (area) {
        case NcaKeyArea::kApplication:
            return "key_area_key_application";
        case NcaKeyArea::kOcean:
            return "key_area_key_ocean";
        case NcaKeyArea::kSystem:
            return "key_area_key_system";
    }
    return "key_area_key_application";
}

}  // namespace

Result<Ticket> ReadTicket(const Storage& storage) {
    const Result<std::uint32_t> signature_type = ReadStruct<std::uint32_t>(storage, 0);
    if (!signature_type) {
        return std::unexpected(signature_type.error());
    }
    const Result<std::uint64_t> offset = DataOffset(*signature_type);
    if (!offset) {
        return std::unexpected(offset.error());
    }
    if (!RangeFits(*offset, kDataSize, storage.Size())) {
        return Failf(Errc::kCorrupt, "ticket too small ({:#x} bytes)", storage.Size());
    }
    std::vector<std::byte> data(kDataSize);
    BIWA_RETURN_IF_ERROR(storage.Read(*offset, data));
    Ticket ticket;
    std::memcpy(ticket.encrypted_title_key.data(), data.data() + 0x40, 16);
    ticket.title_key_type = static_cast<std::uint8_t>(data[0x141]);
    ticket.master_key_revision = static_cast<std::uint8_t>(data[0x145]);
    std::memcpy(ticket.rights_id.data(), data.data() + 0x160, 16);
    if (ticket.title_key_type != 0) {
        return Failf(Errc::kUnsupported,
                     "the ticket for rights ID {} is personalised (made for one console); dump the game with "
                     "console-specific data removed",
                     ToHex(ticket.rights_id));
    }
    return ticket;
}

Result<Aes128Key> DecryptTitleKey(const Ticket& ticket, const KeySet& keys, unsigned master_key_revision) {
    const Result<Aes128Key> kek = keys.Get<16>(IndexedKeyName("titlekek", master_key_revision));
    if (!kek) {
        return std::unexpected(kek.error());
    }
    std::array<std::byte, 16> block{};
    std::memcpy(block.data(), ticket.encrypted_title_key.data(), 16);
    BIWA_RETURN_IF_ERROR(AesEcbDecrypt(*kek, block));
    Aes128Key key{};
    std::memcpy(key.data(), block.data(), 16);
    return key;
}

Result<Aes128Key> NcaCtrKey(const NcaHeader& header, const KeySet& keys, const Ticket* ticket) {
    const unsigned revision = header.MasterKeyRevision();
    if (header.HasRightsId()) {
        if (ticket == nullptr) {
            return Failf(Errc::kMissingKey, "no ticket for rights ID {}", ToHex(header.rights_id));
        }
        if (!std::ranges::equal(ticket->rights_id, header.rights_id)) {
            return Failf(Errc::kInternal, "ticket for rights ID {} given for an NCA with rights ID {}",
                         ToHex(ticket->rights_id), ToHex(header.rights_id));
        }
        return DecryptTitleKey(*ticket, keys, revision);
    }
    const Result<Aes128Key> kaek = keys.Get<16>(IndexedKeyName(KeyAreaName(header.key_area), revision));
    if (!kaek) {
        return std::unexpected(kaek.error());
    }
    std::array<std::byte, 16> block{};
    std::memcpy(block.data(), header.encrypted_key_area[2].data(), 16);
    BIWA_RETURN_IF_ERROR(AesEcbDecrypt(*kaek, block));
    Aes128Key key{};
    std::memcpy(key.data(), block.data(), 16);
    return key;
}

}  // namespace biwa::nx
