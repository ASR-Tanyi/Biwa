// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/json.h"

#include <span>
#include <string>
#include <utility>

#include "biwa/core/file.h"

namespace biwa {

Result<Json> ReadJson(const std::filesystem::path& path) {
    Result<std::string> text = ReadFileText(path);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    try {
        return Json::parse(*text);
    } catch (const Json::parse_error& e) {
        const std::u8string name = path.u8string();
        return Failf(Errc::kCorrupt, "{} is not valid JSON (byte {}): {}", std::string(name.begin(), name.end()),
                     e.byte, e.what());
    }
}

Status WriteJson(const std::filesystem::path& path, const Json& value) {
    std::string text;
    try {
        text = value.dump(2, ' ', false, Json::error_handler_t::strict);
    } catch (const Json::type_error& e) {
        return Failf(Errc::kInvalidArgument, "cannot write JSON: {}", e.what());
    }
    text.push_back('\n');
    return WriteFileAtomic(path, std::as_bytes(std::span(text)));
}

}  // namespace biwa
