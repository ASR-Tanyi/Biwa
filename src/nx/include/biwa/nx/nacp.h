// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"

namespace biwa::nx {

inline constexpr std::size_t kNacpSize = 0x4000;

// The 16 title languages, in their order in the NACP; also the suffix of the icon files
// (icon_<language>.dat) in the control NCA.
inline constexpr std::array<std::string_view, 16> kNacpLanguages = {
    "AmericanEnglish", "BritishEnglish",       "Japanese",          "French",
    "German",          "LatinAmericanSpanish", "Spanish",           "Italian",
    "Dutch",           "CanadianFrench",       "Portuguese",        "Russian",
    "Korean",          "TraditionalChinese",   "SimplifiedChinese", "BrazilianPortuguese"};

struct NacpTitle {
    std::string_view language;
    std::string name;
    std::string publisher;
};

// control.nacp: the application's names and the sizes the runtime needs for save data.
struct Nacp {
    std::vector<NacpTitle> titles;  // the languages that have a name
    // Newer NACPs store the titles compressed (DEFLATE); `titles` is then empty for now.
    bool titles_compressed = false;
    std::string display_version;
    std::uint32_t supported_languages = 0;  // bit per language
    std::uint64_t presence_group_id = 0;
    std::uint64_t add_on_content_base_id = 0;
    std::uint64_t save_data_owner_id = 0;
    std::uint64_t user_account_save_data_size = 0;
    std::uint64_t user_account_save_data_journal_size = 0;
    std::uint64_t device_save_data_size = 0;
    std::uint64_t device_save_data_journal_size = 0;
    std::uint64_t bcat_delivery_cache_storage_size = 0;
    std::uint64_t temporary_storage_size = 0;
    std::uint64_t cache_storage_size = 0;
    std::uint64_t cache_storage_journal_size = 0;
};

// Errc::kCorrupt if `bytes` is not 0x4000 bytes.
Result<Nacp> ParseNacp(std::span<const std::byte> bytes);

}  // namespace biwa::nx
