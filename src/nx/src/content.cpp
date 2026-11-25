// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/content.h"

#include <optional>
#include <utility>

#include "biwa/core/hex.h"

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

Result<StoragePtr> OpenSectionFs(const StoragePtr& nca, const NcaHeader& header, const NcaSection& section,
                                 const KeySet& keys, const TicketMap& tickets, HashCheck check) {
    std::optional<Aes128Key> key;
    if (section.encryption != NcaEncryption::kNone) {
        const auto ticket = tickets.find(ToHex(header.rights_id));
        const Result<Aes128Key> ctr_key = NcaCtrKey(header, keys, ticket == tickets.end() ? nullptr : &ticket->second);
        if (!ctr_key) {
            return std::unexpected(ctr_key.error());
        }
        key = *ctr_key;
    }
    Result<StoragePtr> decrypted = OpenNcaSection(nca, section, key);
    if (!decrypted) {
        return decrypted;
    }
    return OpenSectionData(std::move(*decrypted), section, check);
}

}  // namespace biwa::nx
