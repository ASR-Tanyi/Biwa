// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"

namespace biwa::nx {

// Keys from the player's key files (docs/design/03-extract.md, "Inputs"): `prod.keys` with
// lines like `header_key = 0123...` and `title.keys` with `<rights id> = <title key>`.
//
// Values never appear in messages, logs or `Names()`. They are wiped when the set goes away.
class KeySet {
   public:
    KeySet() = default;
    KeySet(const KeySet&) = default;
    KeySet& operator=(const KeySet&);
    KeySet(KeySet&&) noexcept = default;
    KeySet& operator=(KeySet&&) noexcept;
    ~KeySet();

    // Lines are `name = hex`; blank lines and lines starting with ';' or '#' are skipped. Names
    // are case-insensitive. A later line for the same name replaces the earlier one. `source`
    // names the text in error messages.
    static Result<KeySet> Parse(std::string_view text, std::string_view source);
    static Result<KeySet> Load(const std::filesystem::path& path);

    // Adds `other`'s keys, replacing keys with the same name.
    void Merge(const KeySet& other);

    [[nodiscard]] bool Contains(std::string_view name) const;
    [[nodiscard]] std::size_t size() const { return keys_.size(); }
    [[nodiscard]] std::vector<std::string> Names() const;

    // The key, which must be exactly N bytes: Errc::kMissingKey if absent, Errc::kCorrupt if its
    // length is wrong.
    template <std::size_t N>
    [[nodiscard]] Result<std::array<std::uint8_t, N>> Get(std::string_view name) const {
        Result<std::span<const std::uint8_t>> bytes = Find(name, N);
        if (!bytes) {
            return std::unexpected(std::move(bytes).error());
        }
        std::array<std::uint8_t, N> out{};
        std::ranges::copy(*bytes, out.begin());
        return out;
    }

   private:
    [[nodiscard]] Result<std::span<const std::uint8_t>> Find(std::string_view name, std::size_t size) const;
    void Wipe();

    std::map<std::string, std::vector<std::uint8_t>, std::less<>> keys_;
};

// "key_area_key_application" and 15 → "key_area_key_application_0f".
std::string IndexedKeyName(std::string_view base, unsigned index);

}  // namespace biwa::nx
