// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"

namespace biwa::nx {

enum class ContentMetaType : std::uint8_t {
    kSystemProgram = 0x01,
    kApplication = 0x80,
    kPatch = 0x81,
    kAddOnContent = 0x82,
    kDelta = 0x83,
    kDataPatch = 0x84,
};
std::string_view ContentMetaTypeName(ContentMetaType type);

enum class ContentType : std::uint8_t {
    kMeta,
    kProgram,
    kData,
    kControl,
    kHtmlDocument,
    kLegalInformation,
    kDeltaFragment,
};
std::string_view ContentTypeName(ContentType type);

struct ContentRecord {
    std::array<std::uint8_t, 0x20> hash{};  // SHA-256 of the NCA
    std::array<std::uint8_t, 0x10> id{};    // the NCA's name
    std::uint64_t size = 0;
    ContentType type = ContentType::kMeta;
    std::uint8_t id_offset = 0;
};

// A packaged content meta (.cnmt, inside the meta NCA): what a title is and which NCAs it has.
struct Cnmt {
    std::uint64_t title_id = 0;
    std::uint32_t version = 0;
    ContentMetaType type = ContentMetaType::kApplication;
    // From the extended header: the patch's title for an application; the application for a patch
    // or add-on content.
    std::uint64_t related_title_id = 0;
    std::uint32_t required_system_version = 0;       // applications and patches
    std::uint32_t required_application_version = 0;  // applications and add-on content
    std::vector<ContentRecord> contents;
};

// Errc::kCorrupt if the header or the records do not fit.
Result<Cnmt> ParseCnmt(std::span<const std::byte> bytes);

}  // namespace biwa::nx
