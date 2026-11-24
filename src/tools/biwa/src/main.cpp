// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "biwa/cli/run.h"

int main(int argc, char** argv) {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    std::vector<std::string_view> args;
    args.reserve(raw.size());
    for (const char* arg : raw.empty() ? raw : raw.subspan(1)) {  // argv[0] is the program name
        args.emplace_back(arg);
    }
    return biwa::cli::Run(args, std::cout, std::cerr);
}
