// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/version.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>

namespace {

TEST(VersionTest, MatchesProjectVersion) { EXPECT_EQ(biwa::Version(), BIWA_TEST_PROJECT_VERSION); }

TEST(VersionTest, SourceCommitIsHexOrUnknown) {
    const std::string_view commit = biwa::SourceCommit();
    if (commit == "unknown") {
        return;
    }
    EXPECT_EQ(commit.size(), 12u);
    EXPECT_TRUE(std::ranges::all_of(commit, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        << commit;
}

}  // namespace
