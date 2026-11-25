// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/content.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "biwa/core/hex.h"
#include "biwa/nx/patch.h"

namespace biwa::nx {

TicketMap ReadTickets(const Pfs0& package, const TicketErrorFn& on_error) {
    TicketMap tickets;
    for (const Pfs0Entry& entry : package.entries()) {
        if (!entry.name.ends_with(".tik")) {
            continue;
        }
        const Result<StoragePtr> storage = package.OpenEntry(entry);
        const Result<Ticket> ticket =
            storage ? ReadTicket(**storage) : Result<Ticket>(std::unexpected(storage.error()));
        if (!ticket) {
            on_error(entry.name, ticket.error());
            continue;
        }
        tickets.emplace(ToHex(ticket->rights_id), *ticket);
    }
    return tickets;
}

namespace {

// The AES-CTR key for the sections of `header`, if any of them need one.
Result<std::optional<Aes128Key>> SectionKey(const NcaHeader& header, const NcaSection& section, const KeySet& keys,
                                            const TicketMap& tickets) {
    if (section.encryption == NcaEncryption::kNone) {
        return std::optional<Aes128Key>();
    }
    const auto ticket = tickets.find(ToHex(header.rights_id));
    const Result<Aes128Key> key = NcaCtrKey(header, keys, ticket == tickets.end() ? nullptr : &ticket->second);
    if (!key) {
        return std::unexpected(key.error());
    }
    return std::optional<Aes128Key>(*key);
}

}  // namespace

Result<StoragePtr> OpenSectionFs(const StoragePtr& nca, const NcaHeader& header, const NcaSection& section,
                                 const KeySet& keys, const TicketMap& tickets, HashCheck check) {
    const Result<std::optional<Aes128Key>> key = SectionKey(header, section, keys, tickets);
    if (!key) {
        return std::unexpected(key.error());
    }
    Result<StoragePtr> decrypted = OpenNcaSection(nca, section, *key);
    if (!decrypted) {
        return decrypted;
    }
    return OpenSectionData(std::move(*decrypted), section, check);
}

Result<StoragePtr> OpenPatchedSectionFs(const StoragePtr& base_nca, const NcaHeader& base_header,
                                        const StoragePtr& patch_nca, const NcaHeader& patch_header,
                                        const NcaSection& patch_section, const KeySet& keys, const TicketMap& tickets,
                                        HashCheck check) {
    const auto base_section = std::ranges::find(base_header.sections, patch_section.index, &NcaSection::index);
    if (base_section == base_header.sections.end() || base_section->fs_type != patch_section.fs_type) {
        return Failf(Errc::kInvalidArgument, "the base game has no section {} to patch", patch_section.index);
    }
    const Result<std::optional<Aes128Key>> base_key = SectionKey(base_header, *base_section, keys, tickets);
    if (!base_key) {
        return std::unexpected(base_key.error());
    }
    Result<StoragePtr> base = OpenNcaSection(base_nca, *base_section, *base_key);
    if (!base) {
        return std::unexpected(std::move(base).error().WithContext("base game"));
    }
    const Result<std::optional<Aes128Key>> patch_key = SectionKey(patch_header, patch_section, keys, tickets);
    if (!patch_key) {
        return std::unexpected(patch_key.error());
    }
    if (!*patch_key) {
        return Failf(Errc::kCorrupt, "patch section {} is not encrypted", patch_section.index);
    }
    Result<StoragePtr> patched = OpenPatchedSection(std::move(*base), patch_nca, patch_section, **patch_key);
    if (!patched) {
        return patched;
    }
    return OpenSectionData(std::move(*patched), patch_section, check);
}

}  // namespace biwa::nx
