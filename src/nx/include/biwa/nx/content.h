// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "biwa/core/error.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/pfs0.h"
#include "biwa/nx/section.h"
#include "biwa/nx/storage.h"
#include "biwa/nx/ticket.h"

namespace biwa::nx {

// Tickets by rights ID, as lowercase hex.
using TicketMap = std::map<std::string, Ticket, std::less<>>;

// Reads every .tik file in `package`. A ticket that cannot be read is passed to `on_error` with
// its file name, and left out.
using TicketErrorFn = std::function<void(std::string_view name, const Error& error)>;
TicketMap ReadTickets(const Pfs0& package, const TicketErrorFn& on_error);

// The file system of one section of `nca`: decrypted with the title key from `tickets` (or the
// key area when the NCA has no rights ID), then hash-checked unless `check` is kOff.
Result<StoragePtr> OpenSectionFs(const StoragePtr& nca, const NcaHeader& header, const NcaSection& section,
                                 const KeySet& keys, const TicketMap& tickets, HashCheck check = HashCheck::kOn);

// The file system of an update's patch section laid over the base game: `patch_section` of
// `patch_nca`, over the section with the same index in `base_nca`. Keys come from `tickets` (which
// may hold both packages' tickets) or the key areas, as for OpenSectionFs.
Result<StoragePtr> OpenPatchedSectionFs(const StoragePtr& base_nca, const NcaHeader& base_header,
                                        const StoragePtr& patch_nca, const NcaHeader& patch_header,
                                        const NcaSection& patch_section, const KeySet& keys, const TicketMap& tickets,
                                        HashCheck check = HashCheck::kOn);

}  // namespace biwa::nx
