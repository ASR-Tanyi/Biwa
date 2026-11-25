// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "biwa/core/error.h"
#include "biwa/nx/crypto.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// Reads [offset, offset + out.size()) of `raw` and decrypts it with AES-CTR. `raw`'s first byte
// sits at `base_offset` in the NCA (the counter counts from the start of the NCA). Any offset and
// length; the range must be inside `raw`.
Status ReadAesCtr(const Storage& raw, const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t base_offset,
                  std::uint64_t offset, std::span<std::byte> out);

// AES-CTR over `base`, whose first byte sits at `base_offset` in the NCA (the counter counts from
// the start of the NCA).
StoragePtr MakeAesCtrStorage(StoragePtr base, const Aes128Key& key, std::uint64_t counter_upper,
                             std::uint64_t base_offset);

// A section's decrypted bytes. `ctr_key` is needed for AES-CTR sections (NcaCtrKey). Patch (BKTR)
// and sparse sections, and AES-XTS sections, are Errc::kUnsupported here.
Result<StoragePtr> OpenNcaSection(StoragePtr nca, const NcaSection& section, const std::optional<Aes128Key>& ctr_key);

// Where the file system lives inside a decrypted section of `storage_size` bytes: the last layer
// of a HierarchicalSha256 section (PFS0) or the data level of an IVFC section (RomFS).
struct SectionDataRegion {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};
Result<SectionDataRegion> FindSectionData(const NcaSection& section, std::uint64_t storage_size);

enum class HashCheck : std::uint8_t { kOff, kOn };

// The file system bytes of a decrypted section. With HashCheck::kOn, every read is checked against
// the section's hash tree: the top level is read here and checked against the master hash in the
// header, and each block below is checked when it is read (Errc::kCorrupt on a mismatch).
// Sections without hashes are returned unchecked. A compressed section is decompressed above the
// hash layer (MakeCompressedStorage).
Result<StoragePtr> OpenSectionData(StoragePtr section_storage, const NcaSection& section,
                                   HashCheck check = HashCheck::kOn);

}  // namespace biwa::nx
