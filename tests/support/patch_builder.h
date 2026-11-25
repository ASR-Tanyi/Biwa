// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "biwa/nx/crypto.h"
#include "support/bucket_tree_builder.h"
#include "support/pfs0_builder.h"

namespace biwa::test {

struct PatchSection {
    std::vector<std::byte> data;  // as stored: update data, relocation table, counter table
    std::array<std::byte, 0x40> patch_info{};
    std::size_t update_bytes = 0;  // bytes of update data at the start of `data`
};

// A patch (BKTR) section that turns `base` into `target` (both plaintext sections, before
// encryption). Blocks of `target` equal to `base` at the same offset come from the base; the rest
// is stored as update data. The update data has two counter ranges: generation 5, then (if there
// is more than one block) a range stored unencrypted. The tables use the plain counter.
// `section_offset` is where the section will sit in the NCA.
inline PatchSection BuildPatchSection(const std::vector<std::byte>& base, const std::vector<std::byte>& target,
                                      const nx::Aes128Key& key, std::uint64_t counter_upper,
                                      std::uint64_t section_offset, std::size_t block = 0x200) {
    struct Relocation {
        std::uint64_t virt;
        std::uint64_t phys;
        std::uint32_t index;
    };
    std::vector<Relocation> relocations;
    std::vector<std::byte> update;
    for (std::size_t at = 0; at < target.size(); at += block) {
        const std::size_t size = std::min(block, target.size() - at);
        const auto from = target.begin() + static_cast<std::ptrdiff_t>(at);
        const bool same = at + size <= base.size() && std::equal(from, from + static_cast<std::ptrdiff_t>(size),
                                                                 base.begin() + static_cast<std::ptrdiff_t>(at));
        const Relocation next = same ? Relocation{at, at, 0} : Relocation{at, update.size(), 1};
        if (!same) {
            update.insert(update.end(), from, from + static_cast<std::ptrdiff_t>(size));
        }
        const bool extends = !relocations.empty() && relocations.back().index == next.index &&
                             relocations.back().phys + (at - relocations.back().virt) == next.phys;
        if (!extends) {
            relocations.push_back(next);
        }
    }
    update.resize((update.size() + 0x1FF) / 0x200 * 0x200);

    PatchSection out;
    out.update_bytes = update.size();
    const std::uint64_t generation_upper = (counter_upper & 0xFFFFFFFF00000000) | 5;
    std::vector<std::vector<std::byte>> counters;
    auto add_counter = [&](std::uint64_t offset, std::uint8_t encryption, std::uint32_t generation) {
        std::vector<std::byte> entry(0x10);
        PutU64(entry, 0, offset);
        entry[8] = static_cast<std::byte>(encryption);
        PutU32(entry, 0x0C, generation);
        counters.push_back(std::move(entry));
    };
    const std::size_t encrypted = update.size() > 0x200 ? update.size() / 0x400 * 0x200 : update.size();
    if (!update.empty()) {
        add_counter(0, 0, 5);
        nx::AesCtrCrypt(key, nx::NcaCounter(generation_upper, section_offset), std::span(update).first(encrypted));
    }
    if (encrypted < update.size()) {
        add_counter(encrypted, 1, 0);
    }

    std::vector<std::vector<std::byte>> raw_relocations;
    for (const Relocation& r : relocations) {
        std::vector<std::byte> entry(0x14);
        PutU64(entry, 0, r.virt);
        PutU64(entry, 8, r.phys);
        PutU32(entry, 0x10, r.index);
        raw_relocations.push_back(std::move(entry));
    }
    std::vector<std::byte> relocation_tree = BuildBucketTree(raw_relocations, 0x14, target.size());
    std::vector<std::byte> counter_tree = BuildBucketTree(counters, 0x10, update.size());
    const std::uint64_t relocation_at = update.size();
    const std::uint64_t counter_at = relocation_at + relocation_tree.size();
    nx::AesCtrCrypt(key, nx::NcaCounter(counter_upper, section_offset + relocation_at), relocation_tree);
    nx::AesCtrCrypt(key, nx::NcaCounter(counter_upper, section_offset + counter_at), counter_tree);

    out.data = std::move(update);
    out.data.insert(out.data.end(), relocation_tree.begin(), relocation_tree.end());
    out.data.insert(out.data.end(), counter_tree.begin(), counter_tree.end());
    const auto relocation_info = BucketTreeInfoBytes(relocation_at, relocation_tree.size(), relocations.size());
    const auto counter_info = BucketTreeInfoBytes(counter_at, counter_tree.size(), counters.size());
    std::ranges::copy(relocation_info, out.patch_info.begin());
    std::ranges::copy(counter_info, out.patch_info.begin() + 0x20);
    return out;
}

}  // namespace biwa::test
