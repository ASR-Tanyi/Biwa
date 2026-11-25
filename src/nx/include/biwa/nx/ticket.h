// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstdint>

#include "biwa/core/error.h"
#include "biwa/nx/crypto.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// An eShop ticket (.tik): which content it unlocks and its title key.
//
//   0x000  signature type (u32), signature, padding  (0x140 bytes for RSA-2048 / SHA-256)
//   +0x040 title key block (0x100; a common ticket keeps the encrypted title key in the first 16)
//   +0x141 title key type (0 common, 1 personalised)
//   +0x145 master key revision
//   +0x160 rights ID (16)
struct Ticket {
    std::array<std::uint8_t, 16> rights_id{};
    Aes128Key encrypted_title_key{};
    std::uint8_t title_key_type = 0;
    std::uint8_t master_key_revision = 0;
};

// Errc::kUnsupported for personalised tickets (their title key is encrypted for one console).
Result<Ticket> ReadTicket(const Storage& storage);

// The title key, decrypted with titlekek_<revision>.
Result<Aes128Key> DecryptTitleKey(const Ticket& ticket, const KeySet& keys, unsigned master_key_revision);

// The AES-CTR key for an NCA's sections: with a rights ID, the title key from `ticket` (which must
// be the NCA's ticket); without, key area entry 2 decrypted with key_area_key_<area>_<revision>.
Result<Aes128Key> NcaCtrKey(const NcaHeader& header, const KeySet& keys, const Ticket* ticket);

}  // namespace biwa::nx
