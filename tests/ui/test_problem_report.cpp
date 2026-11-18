// Reporting problems: the issue's text, and noticing a run that didn't end normally.

#include "util/ProblemReport.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <gtest/gtest.h>

namespace {

using namespace kiebitz::app;

TEST(ProblemReport, AnIssueWithTheVersionAndNothingPersonal) {
    const QUrl url = problems::reportUrl(problems::Kind::Problem);
    EXPECT_EQ(url.host(), QStringLiteral("github.com"));
    EXPECT_EQ(url.path(), QStringLiteral("/lukas-ott/kiebitz/issues/new"));
    const QString body = QUrlQuery(url).queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
    EXPECT_TRUE(body.contains(QStringLiteral("What happened?")));
    EXPECT_TRUE(body.endsWith(problems::systemLine()));
    EXPECT_TRUE(problems::systemLine().startsWith(
        QStringLiteral("Kiebitz ") + QCoreApplication::applicationVersion() + " · macOS "));
    EXPECT_FALSE(body.contains(qEnvironmentVariable("USER"))); // no user name

    const QUrl crash = problems::reportUrl(problems::Kind::Crash);
    EXPECT_EQ(QUrlQuery(crash).queryItemValue(QStringLiteral("title"), QUrl::FullyDecoded),
              QStringLiteral("Kiebitz quit unexpectedly"));
}

TEST(ProblemReport, ARunThatDidntEndIsNoticedOnce) {
    const QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    EXPECT_FALSE(problems::startRun(settings)); // the first run
    problems::finishRun(settings);
    EXPECT_FALSE(problems::startRun(settings)); // quit normally before
    // No finishRun: it crashed or was forced to quit.
    EXPECT_TRUE(problems::startRun(settings));
    problems::finishRun(settings);
    EXPECT_FALSE(problems::startRun(settings));
}

} // namespace
