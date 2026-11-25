// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstddef>
#include <cstdint>

#include "biwa/core/error.h"
#include "biwa/nx/bucket_tree.h"
#include "biwa/nx/crypto.h"
#include "biwa/nx/nca.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

// An update's patch (BKTR) section holds only what changed. Its patch info (0x40 bytes at 0x100 in
// the FS header) points to two bucket trees inside the section: the relocation table, which maps
// each range of the patched section to the base game's section or to the update's own data, and
// the counter table, which gives each range of the update's data its AES-CTR counter generation.

inline constexpr std::size_t kAesCtrExEntrySize = 0x10;  // offset, encryption (u8), reserved, generation (s32)
inline constexpr std::size_t kIndirectEntrySize = 0x14;  // virtual offset, physical offset, storage index (s32)

// AES-CTR-Ex over `raw`, the patch section as stored, whose first byte sits at `base_offset` in
// the NCA. In each range of `tree` the low 32 bits of the counter's upper half are that entry's
// generation (or the range is not encrypted); past the tree's end it is plain AES-CTR.
StoragePtr MakeAesCtrExStorage(StoragePtr raw, const Aes128Key& key, std::uint64_t counter_upper,
                               std::uint64_t base_offset, BucketTree tree);

// The patched bytes: each range of `tree` is read from `base` (storage index 0) or `patch` (1).
// Errc::kCorrupt if an entry points outside its storage or the tree does not start at 0.
Result<StoragePtr> MakeIndirectStorage(StoragePtr base, StoragePtr patch, BucketTree tree);

// The patched section, before its hash layer: `patch_section` of `patch_nca`, decrypted with
// AES-CTR-Ex and `key`, laid over `base_section` (the base game's matching section, decrypted, as
// OpenNcaSection gives it). Open its file system with OpenSectionData and `patch_section`.
Result<StoragePtr> OpenPatchedSection(StoragePtr base_section, StoragePtr patch_nca, const NcaSection& patch_section,
                                      const Aes128Key& key);

}  // namespace biwa::nx
