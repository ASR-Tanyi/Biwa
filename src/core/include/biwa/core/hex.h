// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "biwa/core/error.h"

namespace biwa {

// Lower-case hex, two digits per byte.
std::string ToHex(std::span<const std::uint8_t> bytes);

// Parses an even number of hex digits (either case). Anything else is Errc::kInvalidArgument;
// the message gives the position, never the text, because keys are parsed with this.
Result<std::vector<std::uint8_t>> ParseHex(std::string_view text);

}  // namespace biwa
