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
        PutU64(header, fs + 0x140, section.counter_upper);
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
