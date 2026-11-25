// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"
#include "biwa/nx/keys.h"
#include "biwa/nx/storage.h"

namespace biwa::nx {

inline constexpr std::uint64_t kNcaHeaderSize = 0xC00;  // main header + 4 section headers
inline constexpr std::uint64_t kNcaMediaUnit = 0x200;   // section offsets count in these
inline constexpr std::size_t kNcaSectionCount = 4;

enum class NcaContentType : std::uint8_t { kProgram, kMeta, kControl, kManual, kData, kPublicData };
enum class NcaKeyArea : std::uint8_t { kApplication, kOcean, kSystem };
enum class NcaFsType : std::uint8_t { kRomFs, kPartitionFs };
enum class NcaHashType : std::uint8_t {
    kAuto,
    kNone,
    kHierarchicalSha256,
    kHierarchicalIntegrity,
    kAutoSha3,
    kHierarchicalSha3256,
    kHierarchicalIntegritySha3,
};
enum class NcaEncryption : std::uint8_t {
    kAuto,
    kNone,
    kAesXts,
    kAesCtr,
    kAesCtrEx,
    kAesCtrSkipLayerHash,
    kAesCtrExSkipLayerHash,
};

std::string_view ContentTypeName(NcaContentType type);
std::string_view FsTypeName(NcaFsType type);
std::string_view HashTypeName(NcaHashType type);
std::string_view EncryptionName(NcaEncryption encryption);

// One section's header (0x200 bytes at 0x400 + index * 0x200). The hash, patch, sparse and
// compression areas stay raw; the layers that need them parse them.
struct NcaSection {
    unsigned index = 0;
    std::uint64_t offset = 0;  // in the NCA
    std::uint64_t size = 0;
    std::uint16_t version = 0;
    NcaFsType fs_type = NcaFsType::kRomFs;
    NcaHashType hash_type = NcaHashType::kNone;
    NcaEncryption encryption = NcaEncryption::kNone;
    std::uint64_t counter_upper = 0;  // the upper 8 bytes of the AES-CTR counter (generation, secure value)
    std::array<std::byte, 0xF8> hash_data{};
    std::array<std::byte, 0x40> patch_info{};
    std::array<std::byte, 0x30> sparse_info{};
    std::array<std::byte, 0x28> compression_info{};

    [[nodiscard]] bool HasPatch() const;  // BKTR: indirect + AES-CTR-Ex tables present
    [[nodiscard]] bool HasSparse() const;
    [[nodiscard]] bool HasCompression() const;
};

struct NcaHeader {
    char version = '3';             // the digit of "NCA3"
    std::uint8_t distribution = 0;  // 0 download, 1 game card
    NcaContentType content_type = NcaContentType::kProgram;
    std::uint8_t key_generation = 0;  // max(old, new) as stored
    NcaKeyArea key_area = NcaKeyArea::kApplication;
    std::uint64_t content_size = 0;
    std::uint64_t program_id = 0;
    std::uint32_t content_index = 0;
    std::uint32_t sdk_addon_version = 0;
    std::array<std::uint8_t, 16> rights_id{};
    std::array<std::array<std::uint8_t, 16>, 4> encrypted_key_area{};
    std::vector<NcaSection> sections;  // present sections only, in index order

    // The master key revision the keys use: key_generation - 1, or 0 for generations 0 and 1.
    [[nodiscard]] unsigned MasterKeyRevision() const { return key_generation == 0 ? 0 : key_generation - 1u; }
    [[nodiscard]] bool HasRightsId() const;
    [[nodiscard]] const NcaSection* Section(unsigned index) const;
};

// Decrypts the header with `header_key` (AES-XTS) and checks it: magic NCA3 (NCA2 and NCA0 are
// Errc::kUnsupported), section headers matching their hashes, sections inside the storage.
// A wrong header key shows up as a bad magic (Errc::kCorrupt).
Result<NcaHeader> ReadNcaHeader(const Storage& storage, const KeySet& keys);

// The same, from the 0xC00 header bytes already decrypted.
Result<NcaHeader> ParseNcaHeader(std::span<const std::byte> header, std::uint64_t storage_size);

}  // namespace biwa::nx
