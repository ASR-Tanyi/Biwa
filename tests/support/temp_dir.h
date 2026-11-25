// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace biwa::test {

// A fresh directory under the system's temporary directory, removed with everything in it when
// the object goes away.
class TempDir {
   public:
    explicit TempDir(std::string_view prefix = "biwa-test") {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                (std::string(prefix) + "-" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const { return path_ / name; }

   private:
    std::filesystem::path path_;
};

}  // namespace biwa::test
