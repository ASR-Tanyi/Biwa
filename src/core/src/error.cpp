// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/error.h"

namespace biwa {

std::string_view ErrcName(Errc code) {
    switch (code) {
        case Errc::kInvalidArgument:
            return "invalid argument";
        case Errc::kNotFound:
            return "not found";
        case Errc::kIo:
            return "i/o";
        case Errc::kCorrupt:
            return "corrupt";
        case Errc::kMissingKey:
            return "missing key";
        case Errc::kUnsupported:
            return "unsupported";
        case Errc::kInternal:
            return "internal";
    }
    return "unknown";
}

std::ostream& operator<<(std::ostream& os, const Error& error) {
    return os << ErrcName(error.code()) << ": " << error.message();
}

}  // namespace biwa
