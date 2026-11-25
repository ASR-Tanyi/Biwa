// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/nacp.h"

#include <cstring>

namespace biwa::nx {
namespace {

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

std::string Text(std::span<const std::byte> bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return std::string(text.substr(0, text.find('\0')));
}

}  // namespace

Result<Nacp> ParseNacp(std::span<const std::byte> bytes) {
    if (bytes.size() != kNacpSize) {
        return Failf(Errc::kCorrupt, "NACP: {} bytes instead of {:#x}", bytes.size(), kNacpSize);
    }
    Nacp nacp;
    nacp.titles_compressed = Load<std::uint8_t>(bytes, 0x3215) != 0;
    for (std::size_t i = 0; i < kNacpLanguages.size() && !nacp.titles_compressed; ++i) {
        // 16 x {name (0x200), publisher (0x100)}
        const std::span<const std::byte> title = bytes.subspan(i * 0x300, 0x300);
        NacpTitle entry{kNacpLanguages[i], Text(title.first(0x200)), Text(title.subspan(0x200))};
        if (!entry.name.empty()) {
            nacp.titles.push_back(std::move(entry));
        }
    }
    nacp.supported_languages = Load<std::uint32_t>(bytes, 0x302C);
    nacp.presence_group_id = Load<std::uint64_t>(bytes, 0x3038);
    nacp.display_version = Text(bytes.subspan(0x3060, 0x10));
    nacp.add_on_content_base_id = Load<std::uint64_t>(bytes, 0x3070);
    nacp.save_data_owner_id = Load<std::uint64_t>(bytes, 0x3078);
    nacp.user_account_save_data_size = Load<std::uint64_t>(bytes, 0x3080);
    nacp.user_account_save_data_journal_size = Load<std::uint64_t>(bytes, 0x3088);
    nacp.device_save_data_size = Load<std::uint64_t>(bytes, 0x3090);
    nacp.device_save_data_journal_size = Load<std::uint64_t>(bytes, 0x3098);
    nacp.bcat_delivery_cache_storage_size = Load<std::uint64_t>(bytes, 0x30A0);
    nacp.temporary_storage_size = Load<std::uint64_t>(bytes, 0x3168);
    nacp.cache_storage_size = Load<std::uint64_t>(bytes, 0x3170);
    nacp.cache_storage_journal_size = Load<std::uint64_t>(bytes, 0x3178);
    return nacp;
}

}  // namespace biwa::nx
