// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "biwa/core/sha256.h"
#include "biwa/nx/crypto.h"
#include "biwa/nx/nca.h"
#include "support/pfs0_builder.h"

namespace biwa::test {

// Made-up keys for synthetic NCAs. They are not real keys.
inline nx::AesXtsKey TestHeaderKey() {
    nx::AesXtsKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return key;
}

inline nx::Aes128Key TestKey(std::uint8_t seed) {
    nx::Aes128Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(std::size_t{seed} * 17 + i);
    }
    return key;
}

struct NcaBuildSection {
    nx::NcaFsType fs_type = nx::NcaFsType::kPartitionFs;
    nx::NcaHashType hash_type = nx::NcaHashType::kNone;
    nx::NcaEncryption encryption = nx::NcaEncryption::kNone;
    std::uint64_t counter_upper = 0;
    std::array<std::byte, 0xF8> hash_data{};
    std::array<std::byte, 0x40> patch_info{};
    std::array<std::byte, 0x30> sparse_info{};
    std::array<std::byte, 0x28> compression_info{};
    std::vector<std::byte> data;  // the section's plaintext, padded to 0x200 by the builder
};

struct NcaBuildOptions {
    nx::NcaContentType content_type = nx::NcaContentType::kProgram;
    std::uint8_t key_generation = 0;  // written to the new field; the old field gets min(gen, 2)
    std::uint8_t key_area_index = 0;
    std::uint64_t program_id = 0x0100000000001000;
    std::array<std::uint8_t, 16> rights_id{};
    std::array<nx::Aes128Key, 4> key_area{};  // plaintext; encrypted with key_area_key
    nx::Aes128Key key_area_key{};
    // The section key used for AES-CTR data; normally key_area[2], or a title key.
    std::optional<nx::Aes128Key> ctr_key;
    char magic_digit = '3';
};

// Builds an NCA: header encrypted with `header_key`, sections after it (0x200-aligned), CTR
// sections encrypted with the counter for their absolute offsets, as real NCAs are.
inline std::vector<std::byte> BuildNca(const NcaBuildOptions& options, const std::vector<NcaBuildSection>& sections,
                                       const nx::AesXtsKey& header_key = TestHeaderKey()) {
    std::size_t offset = nx::kNcaHeaderSize;
    std::vector<std::size_t> offsets;
    for (const NcaBuildSection& section : sections) {
        offsets.push_back(offset);
        offset += (section.data.size() + 0x1FF) / 0x200 * 0x200;
    }
    std::vector<std::byte> out(offset);
    std::vector<std::byte> header(nx::kNcaHeaderSize);

    const char magic[4] = {'N', 'C', 'A', options.magic_digit};
    std::memcpy(header.data() + 0x200, magic, 4);
    header[0x205] = static_cast<std::byte>(options.content_type);
    header[0x206] = static_cast<std::byte>(std::min<std::uint8_t>(options.key_generation, 2));
    header[0x207] = static_cast<std::byte>(options.key_area_index);
    PutU64(header, 0x208, out.size());
    PutU64(header, 0x210, options.program_id);
    header[0x220] = static_cast<std::byte>(options.key_generation);
    std::memcpy(header.data() + 0x230, options.rights_id.data(), 16);

    for (std::size_t i = 0; i < 4; ++i) {
        std::array<std::byte, 16> block{};
        std::memcpy(block.data(), options.key_area[i].data(), 16);
        if (!nx::AesEcbEncrypt(options.key_area_key, block)) {
            return {};
        }
        std::memcpy(header.data() + 0x300 + i * 16, block.data(), 16);
    }

    for (std::size_t i = 0; i < sections.size(); ++i) {
        const NcaBuildSection& section = sections[i];
        const std::size_t padded = (section.data.size() + 0x1FF) / 0x200 * 0x200;
        PutU32(header, 0x240 + i * 0x10, static_cast<std::uint32_t>(offsets[i] / 0x200));
        PutU32(header, 0x244 + i * 0x10, static_cast<std::uint32_t>((offsets[i] + padded) / 0x200));

        const std::size_t fs = 0x400 + i * 0x200;
        header[fs + 0] = std::byte{2};  // version
        header[fs + 2] = static_cast<std::byte>(section.fs_type);
        header[fs + 3] = static_cast<std::byte>(section.hash_type);
        header[fs + 4] = static_cast<std::byte>(section.encryption);
        std::memcpy(header.data() + fs + 0x8, section.hash_data.data(), section.hash_data.size());
        std::memcpy(header.data() + fs + 0x100, section.patch_info.data(), section.patch_info.size());
        PutU64(header, fs + 0x140, section.counter_upper);
        std::memcpy(header.data() + fs + 0x148, section.sparse_info.data(), section.sparse_info.size());
        std::memcpy(header.data() + fs + 0x178, section.compression_info.data(), section.compression_info.size());
        const Sha256Digest hash = Sha256Of(std::span(header).subspan(fs, 0x200));
        std::memcpy(header.data() + 0x280 + i * 0x20, hash.data(), hash.size());

        std::memcpy(out.data() + offsets[i], section.data.data(), section.data.size());
        if (section.encryption == nx::NcaEncryption::kAesCtr) {
            const nx::Aes128Key key = options.ctr_key.value_or(options.key_area[2]);
            nx::AesCtrCrypt(key, nx::NcaCounter(section.counter_upper, offsets[i]),
                            std::span(out).subspan(offsets[i], padded));
        }
    }

    if (!nx::AesXtsEncrypt(header_key, header, 0, nx::kNcaMediaUnit)) {
        return {};
    }
    std::memcpy(out.data(), header.data(), header.size());
    return out;
}

// SHA-256 of each `block_size` block of `data`; with `pad`, a short last block is hashed as if
// filled with zeros (IVFC).
inline std::vector<std::byte> HashBlocks(std::span<const std::byte> data, std::size_t block_size, bool pad) {
    std::vector<std::byte> out;
    for (std::size_t at = 0; at < data.size(); at += block_size) {
        std::vector<std::byte> block(
            data.begin() + static_cast<std::ptrdiff_t>(at),
            data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), at + block_size)));
        if (pad) {
            block.resize(block_size);
        }
        const Sha256Digest digest = Sha256Of(block);
        const auto* bytes = reinterpret_cast<const std::byte*>(digest.data());
        out.insert(out.end(), bytes, bytes + digest.size());
    }
    return out;
}

// A section's plaintext with a hash tree over a payload, and the hash data that describes it.
struct HashedSection {
    std::array<std::byte, 0xF8> hash_data{};
    std::vector<std::byte> data;
    std::size_t payload_offset = 0;  // where the payload starts in `data`
};

// HierarchicalSha256 with two layers: the hash table at 0, `payload` at the next 0x200 boundary.
inline HashedSection BuildSha256Section(const std::vector<std::byte>& payload, std::uint32_t block_size = 0x1000) {
    const std::vector<std::byte> table = HashBlocks(payload, block_size, false);
    HashedSection out;
    out.payload_offset = (table.size() + 0x1FF) / 0x200 * 0x200;
    out.data.resize(out.payload_offset);
    std::ranges::copy(table, out.data.begin());
    out.data.insert(out.data.end(), payload.begin(), payload.end());

    std::vector<std::byte> bytes(0xF8);
    const Sha256Digest master = Sha256Of(table);
    std::memcpy(bytes.data(), master.data(), master.size());
    PutU32(bytes, 0x20, block_size);
    PutU32(bytes, 0x24, 2);  // layers
    PutU64(bytes, 0x28, 0);
    PutU64(bytes, 0x30, table.size());
    PutU64(bytes, 0x38, out.payload_offset);
    PutU64(bytes, 0x40, payload.size());
    std::ranges::copy(bytes, out.hash_data.begin());
    return out;
}

// IVFC with six levels of 2^block_order-byte blocks: hash levels 0 to 4 first, then `payload`,
// each starting on a block boundary. The payload must be small enough for level 0 to be one block.
inline HashedSection BuildIvfcSection(const std::vector<std::byte>& payload, std::uint32_t block_order = 9) {
    const std::size_t block = std::size_t{1} << block_order;
    std::array<std::vector<std::byte>, 6> levels;
    levels[5] = payload;
    for (std::size_t i = 5; i > 0; --i) {
        levels[i - 1] = HashBlocks(levels[i], block, true);
    }
    const std::vector<std::byte> master = HashBlocks(levels[0], block, true);

    HashedSection out;
    std::vector<std::byte> bytes(0xF8);
    PutU32(bytes, 0x00, 0x43465649);  // "IVFC"
    PutU32(bytes, 0x04, 0x20000);
    PutU32(bytes, 0x08, 0x20);
    PutU32(bytes, 0x0C, 7);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const std::size_t at = 0x10 + i * 0x18;
        PutU64(bytes, at, out.data.size());
        PutU64(bytes, at + 8, levels[i].size());
        PutU32(bytes, at + 0x10, block_order);
        if (i == 5) {
            out.payload_offset = out.data.size();
        }
        out.data.insert(out.data.end(), levels[i].begin(), levels[i].end());
        out.data.resize((out.data.size() + block - 1) / block * block);
    }
    out.data.resize(out.payload_offset + payload.size());
    std::memcpy(bytes.data() + 0xC0, master.data(), 0x20);
    std::ranges::copy(bytes, out.hash_data.begin());
    return out;
}

// A common ticket (RSA-2048 / SHA-256 signature type, signature left zero) for `rights_id`, holding
// `title_key` encrypted with `titlekek`.
inline std::vector<std::byte> BuildTicket(const std::array<std::uint8_t, 16>& rights_id, const nx::Aes128Key& title_key,
                                          const nx::Aes128Key& titlekek, std::uint8_t key_type = 0,
                                          std::uint8_t revision = 0) {
    std::vector<std::byte> out(0x2C0);
    PutU32(out, 0, 0x010004);
    std::array<std::byte, 16> block{};
    std::memcpy(block.data(), title_key.data(), 16);
    if (!nx::AesEcbEncrypt(titlekek, block)) {
        return {};
    }
    std::memcpy(out.data() + 0x180, block.data(), 16);
    out[0x281] = static_cast<std::byte>(key_type);
    out[0x285] = static_cast<std::byte>(revision);
    std::memcpy(out.data() + 0x2A0, rights_id.data(), 16);
    return out;
}

// A KeySet with the test header key and `key_area_key_application_<revision>`.
inline std::string TestKeysText(const nx::Aes128Key& key_area_key, unsigned revision) {
    std::string text = "header_key = ";
    for (const std::uint8_t b : TestHeaderKey()) {
        text += std::format("{:02x}", b);
    }
    text += std::format("\nkey_area_key_application_{:02x} = ", revision);
    for (const std::uint8_t b : key_area_key) {
        text += std::format("{:02x}", b);
    }
    text += '\n';
    return text;
}

}  // namespace biwa::test
