#include "kiebitz/core/version.hpp"

#include <gtest/gtest.h>

TEST(Version, IsNotEmpty) {
    EXPECT_FALSE(kiebitz::core::version().empty());
}

TEST(Version, ParsesTagsAndCompares) {
    using kiebitz::core::parseVersion;
    using kiebitz::core::Version;
    EXPECT_EQ(parseVersion("v0.3.0"), (Version{0, 3, 0}));
    EXPECT_EQ(parseVersion("1.10.2-rc1"), (Version{1, 10, 2}));
    EXPECT_FALSE(parseVersion("1.2").has_value());
    EXPECT_FALSE(parseVersion("latest").has_value());
    EXPECT_LT(*parseVersion("0.9.9"), *parseVersion("0.10.0"));
    EXPECT_TRUE(parseVersion(kiebitz::core::version()).has_value()); // the app's own
}

TEST(Version, OffersTheNewestNewerRelease) {
    using kiebitz::core::pickUpdate;
    using kiebitz::core::Release;
    const std::vector<Release> releases{
        {"v1.1.0-beta", true}, {"v1.0.0", false}, {"v0.4.0", true}, {"v0.3.0", true}};
    // Before 1.0: pre-releases count, the newest newer one wins.
    EXPECT_EQ(pickUpdate({0, 3, 0}, releases), 0U);
    // From 1.0 on: only proper releases.
    EXPECT_FALSE(pickUpdate({1, 0, 0}, releases).has_value());
    EXPECT_EQ(pickUpdate({0, 3, 0}, {{"v0.4.0", true}, {"v0.3.0", true}}), 0U);
    EXPECT_FALSE(pickUpdate({0, 4, 0}, {{"v0.4.0", true}, {"nonsense", false}}).has_value());
}
