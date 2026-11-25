// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/nx/cnmt.h"

#include <cstring>

namespace biwa::nx {
namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kRecordSize = 0x38;

template <class T>
T Load(std::span<const std::byte> bytes, std::size_t at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

}  // namespace

std::string_view ContentMetaTypeName(ContentMetaType type) {
    switch (type) {
        case ContentMetaType::kSystemProgram:
            return "system program";
        case ContentMetaType::kApplication:
            return "application";
        case ContentMetaType::kPatch:
            return "patch";
        case ContentMetaType::kAddOnContent:
            return "add-on content";
        case ContentMetaType::kDelta:
            return "delta";
        case ContentMetaType::kDataPatch:
            return "data patch";
    }
    return "unknown";
}

std::string_view ContentTypeName(ContentType type) {
    switch (type) {
        case ContentType::kMeta:
            return "meta";
        case ContentType::kProgram:
            return "program";
        case ContentType::kData:
            return "data";
        case ContentType::kControl:
            return "control";
        case ContentType::kHtmlDocument:
            return "html document";
        case ContentType::kLegalInformation:
            return "legal information";
        case ContentType::kDeltaFragment:
            return "delta fragment";
    }
    return "unknown";
}

Result<Cnmt> ParseCnmt(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return Fail(Errc::kCorrupt, "CNMT: too small");
    }
    // title ID, version, type, reserved, extended header size (u16), content count (u16), ...
    Cnmt cnmt;
    cnmt.title_id = Load<std::uint64_t>(bytes, 0x00);
    cnmt.version = Load<std::uint32_t>(bytes, 0x08);
    cnmt.type = static_cast<ContentMetaType>(Load<std::uint8_t>(bytes, 0x0C));
    const std::size_t extended = Load<std::uint16_t>(bytes, 0x0E);
    const std::size_t count = Load<std::uint16_t>(bytes, 0x10);
    const std::size_t records = kHeaderSize + extended;
    if (records + count * kRecordSize > bytes.size()) {
        return Failf(Errc::kCorrupt, "CNMT: {} content records do not fit", count);
    }

    const std::span<const std::byte> ext = bytes.subspan(kHeaderSize, extended);
    switch (cnmt.type) {
        case ContentMetaType::kApplication:  // patch ID, required system version, required application version
            if (ext.size() >= 0x10) {
                cnmt.related_title_id = Load<std::uint64_t>(ext, 0);
                cnmt.required_system_version = Load<std::uint32_t>(ext, 8);
                cnmt.required_application_version = Load<std::uint32_t>(ext, 0x0C);
            }
            break;
        case ContentMetaType::kPatch:  // application ID, required system version, ...
            if (ext.size() >= 0x0C) {
                cnmt.related_title_id = Load<std::uint64_t>(ext, 0);
                cnmt.required_system_version = Load<std::uint32_t>(ext, 8);
            }
            break;
        case ContentMetaType::kAddOnContent:  // application ID, required application version, ...
            if (ext.size() >= 0x0C) {
                cnmt.related_title_id = Load<std::uint64_t>(ext, 0);
                cnmt.required_application_version = Load<std::uint32_t>(ext, 8);
            }
            break;
        default:
            break;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::span<const std::byte> r = bytes.subspan(records + i * kRecordSize, kRecordSize);
        ContentRecord record;
        std::memcpy(record.hash.data(), r.data(), record.hash.size());
        std::memcpy(record.id.data(), r.data() + 0x20, record.id.size());
        std::uint64_t size = 0;
        std::memcpy(&size, r.data() + 0x30, 6);  // 48-bit size
        record.size = size;
        record.type = static_cast<ContentType>(Load<std::uint8_t>(r, 0x36));
        record.id_offset = Load<std::uint8_t>(r, 0x37);
        cnmt.contents.push_back(record);
    }
    return cnmt;
}

}  // namespace biwa::nx
