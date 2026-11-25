// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <optional>

#include "biwa/core/error.h"
#include "biwa/nx/crypto.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// AES-CTR over `base`, whose first byte sits at `base_offset` in the NCA (the counter counts from
// the start of the NCA).
StoragePtr MakeAesCtrStorage(StoragePtr base, const Aes128Key& key, std::uint64_t counter_upper,
                             std::uint64_t base_offset);

// A section's decrypted bytes. `ctr_key` is needed for AES-CTR sections (NcaCtrKey). Patch
// (BKTR), sparse and compressed sections, and AES-XTS sections, are Errc::kUnsupported here.
Result<StoragePtr> OpenNcaSection(StoragePtr nca, const NcaSection& section, const std::optional<Aes128Key>& ctr_key);

// Where the file system lives inside a decrypted section: the last layer of a
// HierarchicalSha256 section (PFS0) or the data level of an IVFC section (RomFS).
struct SectionDataRegion {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
Result<SectionDataRegion> FindSectionData(const NcaSection& section);

enum class HashCheck : std::uint8_t { kOff, kOn };

// The file system bytes of a decrypted section. With HashCheck::kOn, every read is checked against
// the section's hash tree: the top level is read here and checked against the master hash in the
// header, and each block below is checked when it is read (Errc::kCorrupt on a mismatch).
// Sections without hashes are returned unchecked.
Result<StoragePtr> OpenSectionData(StoragePtr section_storage, const NcaSection& section,
                                   HashCheck check = HashCheck::kOn);

}  // namespace biwa::nx
