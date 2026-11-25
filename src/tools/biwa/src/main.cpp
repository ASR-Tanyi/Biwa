// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "biwa/cli/run.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // `biwa romfs cat` writes file bytes as they are
#endif
    try {
        const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
        std::vector<std::string_view> args;
        args.reserve(raw.size());
        for (const char* arg : raw.empty() ? raw : raw.subspan(1)) {  // argv[0] is the program name
            args.emplace_back(arg);
        }
        return biwa::cli::Run(args, std::cout, std::cerr);
    } catch (const std::exception& e) {
        std::cerr << "biwa: internal error: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "biwa: internal error\n";
    }
    return biwa::cli::kExitInternal;
}
