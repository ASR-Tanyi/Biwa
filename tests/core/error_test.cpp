// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Biwa contributors

#include "biwa/core/error.h"

#include <gtest/gtest.h>

#include <sstream>

namespace {

using biwa::Errc;
using biwa::Error;
using biwa::Result;
using biwa::Status;

Result<int> Half(int value) {
    if (value % 2 != 0) {
        return biwa::Failf(Errc::kInvalidArgument, "{} is odd", value);
    }
    return value / 2;
}

Status CheckQuarter(int value) {
    BIWA_RETURN_IF_ERROR(Half(value));
    BIWA_RETURN_IF_ERROR(Half(value / 2));
    return {};
}

Result<int> Quarter(int value) {
    BIWA_RETURN_IF_ERROR(CheckQuarter(value));
    return value / 4;
}

TEST(ErrorTest, NamesEveryCode) {
    EXPECT_EQ(biwa::ErrcName(Errc::kInvalidArgument), "invalid argument");
    EXPECT_EQ(biwa::ErrcName(Errc::kNotFound), "not found");
    EXPECT_EQ(biwa::ErrcName(Errc::kIo), "i/o");
    EXPECT_EQ(biwa::ErrcName(Errc::kCorrupt), "corrupt");
    EXPECT_EQ(biwa::ErrcName(Errc::kMissingKey), "missing key");
    EXPECT_EQ(biwa::ErrcName(Errc::kUnsupported), "unsupported");
    EXPECT_EQ(biwa::ErrcName(Errc::kInternal), "internal");
}

TEST(ErrorTest, FailfFormatsTheMessage) {
    const Result<int> r = Half(3);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), Error(Errc::kInvalidArgument, "3 is odd"));
}

TEST(ErrorTest, ContextIsPrefixed) {
    Error e = Error(Errc::kCorrupt, "bad magic").WithContext("reading header").WithContext("opening game.nsp");
    EXPECT_EQ(e.code(), Errc::kCorrupt);
    EXPECT_EQ(e.message(), "opening game.nsp: reading header: bad magic");
}

TEST(ErrorTest, StreamsCategoryAndMessage) {
    std::ostringstream os;
    os << Error(Errc::kMissingKey, "titlekek_0f is not in prod.keys");
    EXPECT_EQ(os.str(), "missing key: titlekek_0f is not in prod.keys");
}

TEST(ErrorTest, ReturnIfErrorPassesValuesThrough) {
    const Result<int> r = Quarter(12);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3);
}

TEST(ErrorTest, ReturnIfErrorReturnsTheFirstError) {
    const Result<int> r = Quarter(6);  // 6 / 2 = 3 is odd
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message(), "3 is odd");
    EXPECT_FALSE(CheckQuarter(5).has_value());
}

}  // namespace
