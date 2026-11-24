// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

#include "biwa/core/error.h"

namespace biwa {

// JSON values keep their keys in insertion order, so files Biwa writes read top to bottom
// the way the code builds them.
using Json = nlohmann::ordered_json;

// Errc::kNotFound / kIo for the file, Errc::kCorrupt (with the byte offset) for bad JSON.
Result<Json> ReadJson(const std::filesystem::path& path);

// Two-space indentation and a final newline, written atomically. Strings that are not valid
// UTF-8 are Errc::kInvalidArgument.
Status WriteJson(const std::filesystem::path& path, const Json& value);

}  // namespace biwa
