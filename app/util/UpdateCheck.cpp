#include "UpdateCheck.hpp"

#include "kiebitz/core/version.hpp"
#include "settings/Settings.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>

#include <utility>
#include <vector>

namespace kiebitz::app {
namespace {

constexpr auto kReleases = "https://api.github.com/repos/lukas-ott/kiebitz/releases?per_page=20";
constexpr QLatin1StringView kLastCheck{"updates/lastCheck"};

} // namespace

UpdateCheck::UpdateCheck(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_releases(QString::fromLatin1(kReleases)),
      m_current(QCoreApplication::applicationVersion()) {}

UpdateCheck::~UpdateCheck() {
    if (m_reply != nullptr) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

void UpdateCheck::setSourceForTests(QUrl releases, QString current) {
    m_releases = std::move(releases);
    m_current = std::move(current);
}

void UpdateCheck::check() {
    fetch(true);
}

void UpdateCheck::checkIfDue() {
    if (!Settings::instance().flag(Settings::kCheckUpdates)) {
        return;
    }
    if (QSettings().value(kLastCheck).toDate() == QDate::currentDate()) {
        return;
    }
    fetch(false);
}

void UpdateCheck::fetch(bool asked) {
    if (m_reply != nullptr) {
        return; // one at a time
    }
    QNetworkRequest request(m_releases);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Kiebitz/%1").arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(20000);
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this, asked] {
        QNetworkReply* reply = std::exchange(m_reply, nullptr);
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (asked) {
                emit failed();
            }
            return;
        }
        QSettings().setValue(kLastCheck, QDate::currentDate());
        const QJsonArray list = QJsonDocument::fromJson(reply->readAll()).array();
        std::vector<core::Release> releases;
        for (const auto& value : list) {
            const QJsonObject release = value.toObject();
            // A draft is never offered: no tag, so it never counts (the indexes stay in step).
            const bool draft = release.value(QStringLiteral("draft")).toBool();
            releases.push_back(
                {draft ? std::string()
                       : release.value(QStringLiteral("tag_name")).toString().toStdString(),
                 release.value(QStringLiteral("prerelease")).toBool()});
        }
        const auto current = core::parseVersion(m_current.toStdString());
        const auto pick = current ? core::pickUpdate(*current, releases) : std::nullopt;
        if (!pick) {
            if (asked) {
                emit upToDate();
            }
            return;
        }
        const QJsonObject release = list.at(static_cast<qsizetype>(*pick)).toObject();
        QString version = release.value(QStringLiteral("tag_name")).toString();
        if (version.startsWith(u'v')) {
            version.remove(0, 1);
        }
        emit available(version, QUrl(release.value(QStringLiteral("html_url")).toString()));
    });
}

} // namespace kiebitz::app
