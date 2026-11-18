// The update check: a newer release on GitHub is offered, the same or an older one isn't.

#include "fake_server.hpp"
#include "util/UpdateCheck.hpp"

#include <QSignalSpy>
#include <gtest/gtest.h>

namespace {

using namespace kiebitz::app;

constexpr const char* kReleases = R"([
  {"tag_name": "v0.5.0-beta", "prerelease": true, "draft": true,
   "html_url": "https://example.org/draft"},
  {"tag_name": "v0.4.0", "prerelease": true, "html_url": "https://example.org/v0.4.0"},
  {"tag_name": "v0.3.0", "prerelease": true, "html_url": "https://example.org/v0.3.0"}])";

TEST(UpdateCheck, OffersANewerReleaseAndSaysWhenUpToDate) {
    kiebitz::test::FakeServer server;
    server.pages["/releases"] = kReleases;
    const QUrl source = server.url().resolved(QUrl(QStringLiteral("/releases")));

    UpdateCheck older;
    older.setSourceForTests(source, QStringLiteral("0.3.0"));
    QSignalSpy available(&older, &UpdateCheck::available);
    older.check();
    ASSERT_TRUE(available.wait(10000));
    EXPECT_EQ(available.front().at(0).toString(), QStringLiteral("0.4.0")); // not the draft
    EXPECT_EQ(available.front().at(1).toUrl(), QUrl(QStringLiteral("https://example.org/v0.4.0")));

    UpdateCheck newest;
    newest.setSourceForTests(source, QStringLiteral("0.4.0"));
    QSignalSpy upToDate(&newest, &UpdateCheck::upToDate);
    const QSignalSpy none(&newest, &UpdateCheck::available);
    newest.check();
    ASSERT_TRUE(upToDate.wait(10000));
    EXPECT_TRUE(none.isEmpty());

    UpdateCheck offline;
    offline.setSourceForTests(server.url().resolved(QUrl(QStringLiteral("/missing"))),
                              QStringLiteral("0.3.0"));
    QSignalSpy failed(&offline, &UpdateCheck::failed);
    offline.check();
    EXPECT_TRUE(failed.wait(10000));
}

} // namespace
