#include "ProblemReport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>
#include <QUrlQuery>

namespace kiebitz::app::problems {
namespace {

constexpr auto kNewIssue = "https://github.com/lukas-ott/kiebitz/issues/new";
constexpr QLatin1StringView kRunning{"app/running"};

} // namespace

QString systemLine() {
    return QStringLiteral("Kiebitz %1 · macOS %2 · %3")
        .arg(QCoreApplication::applicationVersion(), QSysInfo::productVersion(),
             QSysInfo::currentCpuArchitecture());
}

QUrl reportUrl(Kind kind) {
    // In English whatever the app's language: the issues are read in English.
    QString title;
    QString body;
    if (kind == Kind::Crash) {
        title = QStringLiteral("Kiebitz quit unexpectedly");
        body = QStringLiteral("**What were you doing when it happened?**\n\n\n"
                              "**Crash report:** please drag the file Finder shows (Kiebitz-….ips) "
                              "into this box.\n\n");
    } else {
        body = QStringLiteral("**What happened?**\n\n\n"
                              "**What did you expect instead?**\n\n\n"
                              "**How can it be repeated?**\n\n\n");
    }
    body += QStringLiteral("---\n") + systemLine();
    QUrl url(QString::fromLatin1(kNewIssue));
    QUrlQuery query;
    if (!title.isEmpty()) {
        query.addQueryItem(QStringLiteral("title"), title);
    }
    query.addQueryItem(QStringLiteral("body"), body);
    url.setQuery(query);
    return url;
}

bool startRun(QSettings& settings) {
    const bool crashed = settings.value(kRunning, false).toBool();
    settings.setValue(kRunning, true);
    settings.sync();
    return crashed;
}

void finishRun(QSettings& settings) {
    settings.setValue(kRunning, false);
    settings.sync();
}

QString latestCrashReport() {
    const QDir reports(QDir::home().filePath(QStringLiteral("Library/Logs/DiagnosticReports")));
    const QFileInfoList found =
        reports.entryInfoList({QStringLiteral("Kiebitz*.ips"), QStringLiteral("Kiebitz*.crash")},
                              QDir::Files, QDir::Time);
    return found.isEmpty() ? QString() : found.front().absoluteFilePath();
}

} // namespace kiebitz::app::problems
