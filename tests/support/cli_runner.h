// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "biwa/cli/run.h"

namespace biwa::test {

struct RunResult {
    int code;
    std::string out;
    std::string err;
};

// Runs the biwa command line in this process.
inline RunResult RunBiwa(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    std::ostringstream out;
    std::ostringstream err;
    const int code = biwa::cli::Run(list, out, err);
    return {code, out.str(), err.str()};
}

inline std::string PathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

}  // namespace biwa::test
