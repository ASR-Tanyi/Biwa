// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/nca.h"

#include <algorithm>
#include <cstring>

#include "biwa/core/sha256.h"
#include "biwa/nx/crypto.h"

namespace biwa::nx {
namespace {

struct RawFsEntry {
    std::uint32_t start;  // media units
    std::uint32_t end;
    std::uint32_t reserved[2];
};

struct RawHeader {
    std::uint8_t signature_fixed[0x100];
    std::uint8_t signature_npdm[0x100];
    std::uint32_t magic;  // 0x200
    std::uint8_t distribution;
    std::uint8_t content_type;
    std::uint8_t key_generation_old;
    std::uint8_t key_area_index;
    std::uint64_t content_size;  // 0x208
    std::uint64_t program_id;
    std::uint32_t content_index;
    std::uint32_t sdk_addon_version;
    std::uint8_t key_generation;  // 0x220
    std::uint8_t signature_key_generation;
    std::uint8_t reserved[0xE];
    std::uint8_t rights_id[0x10];          // 0x230
    RawFsEntry sections[4];                // 0x240
    std::uint8_t section_hashes[4][0x20];  // 0x280
    std::uint8_t key_area[4][0x10];        // 0x300
    std::uint8_t reserved2[0xC0];
};
static_assert(sizeof(RawHeader) == 0x400);

struct RawFsHeader {
    std::uint16_t version;
    std::uint8_t fs_type;
    std::uint8_t hash_type;
    std::uint8_t encryption_type;
    std::uint8_t metadata_hash_type;
    std::uint8_t reserved[2];
    std::byte hash_data[0xF8];         // 0x008
    std::byte patch_info[0x40];        // 0x100
    std::uint64_t counter_upper;       // 0x140
    std::byte sparse_info[0x30];       // 0x148
    std::byte compression_info[0x28];  // 0x178
    std::byte metadata_hash_info[0x30];
    std::byte reserved2[0x30];
};
static_assert(sizeof(RawFsHeader) == 0x200);

constexpr std::uint32_t MagicOf(const char (&text)[5]) {
    return static_cast<std::uint32_t>(text[0]) | static_cast<std::uint32_t>(text[1]) << 8 |
           static_cast<std::uint32_t>(text[2]) << 16 | static_cast<std::uint32_t>(text[3]) << 24;
}

std::uint64_t U64At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint16_t U16At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

}  // namespace

std::string_view ContentTypeName(NcaContentType type) {
    switch (type) {
        case NcaContentType::kProgram:
            return "program";
        case NcaContentType::kMeta:
            return "meta";
        case NcaContentType::kControl:
            return "control";
        case NcaContentType::kManual:
            return "manual";
        case NcaContentType::kData:
            return "data";
        case NcaContentType::kPublicData:
            return "public data";
    }
    return "?";
}

std::string_view FsTypeName(NcaFsType type) { return type == NcaFsType::kRomFs ? "RomFS" : "PFS0"; }

std::string_view HashTypeName(NcaHashType type) {
    switch (type) {
        case NcaHashType::kAuto:
            return "auto";
        case NcaHashType::kNone:
            return "none";
        case NcaHashType::kHierarchicalSha256:
            return "sha256";
        case NcaHashType::kHierarchicalIntegrity:
            return "ivfc";
        case NcaHashType::kAutoSha3:
            return "auto-sha3";
        case NcaHashType::kHierarchicalSha3256:
            return "sha3-256";
        case NcaHashType::kHierarchicalIntegritySha3:
            return "ivfc-sha3";
    }
    return "?";
}

std::string_view EncryptionName(NcaEncryption encryption) {
    switch (encryption) {
        case NcaEncryption::kAuto:
            return "auto";
        case NcaEncryption::kNone:
            return "none";
        case NcaEncryption::kAesXts:
            return "aes-xts";
        case NcaEncryption::kAesCtr:
            return "aes-ctr";
        case NcaEncryption::kAesCtrEx:
            return "aes-ctr-ex";
        case NcaEncryption::kAesCtrSkipLayerHash:
            return "aes-ctr-skip-layer-hash";
        case NcaEncryption::kAesCtrExSkipLayerHash:
            return "aes-ctr-ex-skip-layer-hash";
    }
    return "?";
}

bool NcaSection::HasPatch() const { return U64At(patch_info, 0x08) != 0; }  // indirect table size

bool NcaSection::HasSparse() const { return U16At(sparse_info, 0x28) != 0; }  // generation

bool NcaSection::HasCompression() const { return U64At(compression_info, 0x08) != 0; }  // table size

bool NcaHeader::HasRightsId() const {
    return std::ranges::any_of(rights_id, [](std::uint8_t b) { return b != 0; });
}

const NcaSection* NcaHeader::Section(unsigned index) const {
    const auto it = std::ranges::find(sections, index, &NcaSection::index);
    return it == sections.end() ? nullptr : &*it;
}

Result<NcaHeader> ParseNcaHeader(std::span<const std::byte> header, std::uint64_t storage_size) {
    if (header.size() < kNcaHeaderSize) {
        return Failf(Errc::kCorrupt, "NCA header has {:#x} bytes, needs {:#x}", header.size(), kNcaHeaderSize);
    }
    RawHeader raw{};
    std::memcpy(&raw, header.data(), sizeof(raw));
    if (raw.magic == MagicOf("NCA2") || raw.magic == MagicOf("NCA0")) {
        return Failf(Errc::kUnsupported, "NCA{} is not supported yet, only NCA3", raw.magic == MagicOf("NCA2") ? 2 : 0);
    }
    if (raw.magic != MagicOf("NCA3")) {
        return Fail(Errc::kCorrupt, "bad NCA magic after decryption (a wrong header_key, or not an NCA)");
    }
    if (raw.content_type > static_cast<std::uint8_t>(NcaContentType::kPublicData)) {
        return Failf(Errc::kCorrupt, "unknown NCA content type {}", raw.content_type);
    }
    if (raw.key_area_index > static_cast<std::uint8_t>(NcaKeyArea::kSystem)) {
        return Failf(Errc::kCorrupt, "unknown NCA key area index {}", raw.key_area_index);
    }
    if (raw.content_size != storage_size) {
        return Failf(Errc::kCorrupt, "NCA says it has {:#x} bytes, but its file has {:#x}", raw.content_size,
                     storage_size);
    }

    NcaHeader out;
    out.version = '3';
    out.distribution = raw.distribution;
    out.content_type = static_cast<NcaContentType>(raw.content_type);
    out.key_generation = std::max(raw.key_generation_old, raw.key_generation);
    out.key_area = static_cast<NcaKeyArea>(raw.key_area_index);
    out.content_size = raw.content_size;
    out.program_id = raw.program_id;
    out.content_index = raw.content_index;
    out.sdk_addon_version = raw.sdk_addon_version;
    std::ranges::copy(raw.rights_id, out.rights_id.begin());
    for (std::size_t i = 0; i < 4; ++i) {
        std::ranges::copy(raw.key_area[i], out.encrypted_key_area[i].begin());
    }

    for (unsigned i = 0; i < kNcaSectionCount; ++i) {
        const RawFsEntry& entry = raw.sections[i];
        if (entry.start == 0 && entry.end == 0) {
            continue;
        }
        const std::uint64_t start = std::uint64_t{entry.start} * kNcaMediaUnit;
        const std::uint64_t end = std::uint64_t{entry.end} * kNcaMediaUnit;
        if (start < kNcaHeaderSize || end <= start || end > storage_size) {
            return Failf(Errc::kCorrupt, "NCA section {} ({:#x}..{:#x}) is outside the content (size {:#x})", i, start,
                         end, storage_size);
        }
        const std::span<const std::byte> fs_bytes = header.subspan(0x400 + i * 0x200, 0x200);
        const Sha256Digest hash = Sha256Of(fs_bytes);
        if (!std::ranges::equal(hash, raw.section_hashes[i])) {
            return Failf(Errc::kCorrupt, "NCA section {} header does not match its hash", i);
        }
        RawFsHeader fs{};
        std::memcpy(&fs, fs_bytes.data(), sizeof(fs));
        if (fs.fs_type > static_cast<std::uint8_t>(NcaFsType::kPartitionFs) ||
            fs.hash_type > static_cast<std::uint8_t>(NcaHashType::kHierarchicalIntegritySha3) ||
            fs.encryption_type > static_cast<std::uint8_t>(NcaEncryption::kAesCtrExSkipLayerHash)) {
            return Failf(Errc::kCorrupt, "NCA section {} header has unknown types (fs {}, hash {}, encryption {})", i,
                         fs.fs_type, fs.hash_type, fs.encryption_type);
        }
        NcaSection section;
        section.index = i;
        section.offset = start;
        section.size = end - start;
        section.version = fs.version;
        section.fs_type = static_cast<NcaFsType>(fs.fs_type);
        section.hash_type = static_cast<NcaHashType>(fs.hash_type);
        section.encryption = static_cast<NcaEncryption>(fs.encryption_type);
        section.counter_upper = fs.counter_upper;
        std::ranges::copy(fs.hash_data, section.hash_data.begin());
        std::ranges::copy(fs.patch_info, section.patch_info.begin());
        std::ranges::copy(fs.sparse_info, section.sparse_info.begin());
        std::ranges::copy(fs.compression_info, section.compression_info.begin());
        out.sections.push_back(section);
    }
    return out;
}

Result<NcaHeader> ReadNcaHeader(const Storage& storage, const KeySet& keys) {
    const Result<AesXtsKey> header_key = keys.Get<32>("header_key");
    if (!header_key) {
        return std::unexpected(header_key.error());
    }
    if (storage.Size() < kNcaHeaderSize) {
        return Failf(Errc::kCorrupt, "too small for an NCA ({:#x} bytes)", storage.Size());
    }
    std::vector<std::byte> header(kNcaHeaderSize);
    BIWA_RETURN_IF_ERROR(storage.Read(0, header));
    BIWA_RETURN_IF_ERROR(AesXtsDecrypt(*header_key, header, 0, kNcaMediaUnit));
    return ParseNcaHeader(header, storage.Size());
}

}  // namespace biwa::nx
