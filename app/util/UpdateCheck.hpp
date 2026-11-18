#pragma once

#include <QDate>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace kiebitz::app {

/// Asks GitHub whether a newer Kiebitz has been released (core::pickUpdate decides which).
/// In the background, at most once a day unless asked (Help → Check for Updates…), and not at
/// all when switched off in Settings. Only the list of releases is fetched: nothing about you
/// is sent.
class UpdateCheck : public QObject {
    Q_OBJECT

public:
    explicit UpdateCheck(QObject* parent = nullptr);
    ~UpdateCheck() override;
    UpdateCheck(const UpdateCheck&) = delete;
    UpdateCheck& operator=(const UpdateCheck&) = delete;
    UpdateCheck(UpdateCheck&&) = delete;
    UpdateCheck& operator=(UpdateCheck&&) = delete;

    /// Checks now (asked for: its answer is always reported).
    void check();
    /// Checks if it is on and wasn't done today (a while after the start).
    void checkIfDue();

    /// Tests: where the release list comes from, and the version to compare with.
    void setSourceForTests(QUrl releases, QString current);

signals:
    /// A newer version: its number ("1.0.0") and its release page.
    void available(const QString& version, const QUrl& page);
    /// Asked for, and none: Kiebitz is up to date.
    void upToDate();
    /// Asked for, and GitHub couldn't be reached.
    void failed();

private:
    void fetch(bool asked);

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    QUrl m_releases;
    QString m_current;
};

} // namespace kiebitz::app
