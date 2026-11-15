// Lichess's broadcasts: downloading the monthly files and building their database.

#include "DatabaseManager.hpp"
#include "kiebitz/gamedb/import.hpp"
#include "net/Lichess.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace kiebitz::app {
namespace {

/// "lichess_db_broadcast_2026-10.pgn.zst": the files, and their month.
const QRegularExpression& monthFile() {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^lichess_db_broadcast_(\d{4}-\d{2})\.pgn\.zst$)"));
    return pattern;
}

/// The file names in Lichess's list (one address per line), oldest month first.
QStringList listedFiles(const QByteArray& list) {
    QStringList files;
    for (const QByteArray& line : list.split('\n')) {
        const QString name = QString::fromUtf8(line.trimmed()).section(u'/', -1);
        if (monthFile().match(name).hasMatch()) {
            files << name;
        }
    }
    files.sort(); // the month is in the name: by name is by date
    files.removeDuplicates();
    return files;
}

} // namespace

std::shared_ptr<const gamedb::Database> DatabaseManager::broadcasts() const {
    for (const InstalledDatabase& database : m_databases) {
        if (database.db->info().source == kBroadcastsSource) {
            return database.db;
        }
    }
    return nullptr;
}

QString DatabaseManager::broadcastMonthsFolder() const {
    return QDir::cleanPath(m_root + QStringLiteral("/../Broadcasts"));
}

QStringList DatabaseManager::broadcastMonths() const {
    QStringList months;
    const QStringList files =
        QDir(broadcastMonthsFolder()).entryList({QStringLiteral("*.pgn.zst")}, QDir::Files);
    for (const QString& file : files) {
        if (const auto match = monthFile().match(file); match.hasMatch()) {
            months << match.captured(1);
        }
    }
    months.sort();
    return months;
}

void DatabaseManager::fetchBroadcastList(
    std::function<void(const QStringList& missing, const QString& error)> done) {
    if (m_network == nullptr) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkReply* reply = m_network->get(
        lichess::request(m_broadcastServer.resolved(QUrl(QStringLiteral("/broadcast/list.txt"))),
                         {}, lichess::kTimeoutMs));
    connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, tr("Lichess's database isn't answering. Try again later."));
            return;
        }
        const QStringList have = QDir(broadcastMonthsFolder()).entryList(QDir::Files);
        QStringList missing;
        for (const QString& file : listedFiles(reply->readAll())) {
            if (!have.contains(file)) {
                missing << file;
            }
        }
        done(missing, {});
    });
}

void DatabaseManager::checkBroadcasts(
    std::function<void(int newMonths, const QString& error)> done) {
    fetchBroadcastList([done = std::move(done)](const QStringList& missing, const QString& error) {
        done(error.isEmpty() ? static_cast<int>(missing.size()) : -1, error);
    });
}

int DatabaseManager::updateBroadcasts() {
    if (m_broadcastUpdate.job != 0) {
        return 0;
    }
    const int id = m_nextJob++;
    m_broadcastUpdate = {};
    m_broadcastUpdate.job = id;
    m_jobs.push_back(
        {id, QString::fromLatin1(kBroadcastsSource), tr("Looking for new months"), -1.0, 0});
    emit jobChanged(id);
    emit changed();
    fetchBroadcastList([this, id](const QStringList& missing, const QString& error) {
        if (m_broadcastUpdate.job != id) {
            return;
        }
        const bool cancelled = m_broadcastUpdate.cancelled;
        if (!error.isEmpty() || cancelled) {
            m_broadcastUpdate = {};
            finished(id, cancelled ? QStringLiteral("cancelled") : error);
            return;
        }
        m_broadcastUpdate.months = missing;
        m_broadcastUpdate.total = static_cast<int>(missing.size());
        downloadNextMonth(id);
    });
    return id;
}

void DatabaseManager::downloadNextMonth(int id) {
    BroadcastUpdate& pending = m_broadcastUpdate;
    if (pending.job != id) {
        return;
    }
    if (pending.cancelled) {
        m_broadcastUpdate = {};
        finished(id, QStringLiteral("cancelled"));
        return;
    }
    if (pending.months.isEmpty()) {
        if (pending.total == 0 && broadcasts() != nullptr) {
            m_broadcastUpdate = {};
            finished(id, {}); // nothing new
            return;
        }
        buildBroadcasts(id);
        return;
    }
    const QString name = pending.months.takeFirst();
    const QString folder = broadcastMonthsFolder();
    QDir().mkpath(folder);
    const QString target = QDir(folder).filePath(name);
    const auto file = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_broadcastUpdate = {};
        finished(id, tr("Couldn't save the download."));
        return;
    }
    const int done = pending.total - static_cast<int>(pending.months.size()) - 1;
    const int total = pending.total;
    pending.reply = m_network->get(
        lichess::request(m_broadcastServer.resolved(QUrl(QStringLiteral("/broadcast/") + name))));
    QNetworkReply* reply = pending.reply;
    connect(reply, &QNetworkReply::readyRead, this, [reply, file] {
        const QByteArray chunk = reply->readAll();
        if (file->write(chunk) != chunk.size()) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, id, done, total](qint64 received, qint64 size) {
                const double month =
                    size > 0 ? static_cast<double>(received) / static_cast<double>(size) : 0.0;
                update(id, tr("Downloading month %1 of %2").arg(done + 1).arg(total),
                       (done + month) / std::max(total, 1), 0);
            });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, file, target] {
        reply->deleteLater();
        if (m_broadcastUpdate.job != id) {
            return;
        }
        m_broadcastUpdate.reply = nullptr;
        const bool ok = reply->error() == QNetworkReply::NoError && !m_broadcastUpdate.cancelled;
        if (ok) {
            file->write(reply->readAll());
        }
        file->close();
        if (!ok) {
            file->remove();
            const bool cancelled = m_broadcastUpdate.cancelled;
            m_broadcastUpdate = {};
            finished(id, cancelled ? QStringLiteral("cancelled")
                                   : tr("The download didn't finish. Check the connection "
                                        "and try again."));
            return;
        }
        QFile::remove(target);
        if (!file->rename(target)) {
            file->remove();
            m_broadcastUpdate = {};
            finished(id, tr("Couldn't save the download."));
            return;
        }
        downloadNextMonth(id);
    });
}

void DatabaseManager::buildBroadcasts(int id) {
    // Every month, oldest first, into one new database that then takes the old one's place.
    std::vector<std::string> paths;
    qint64 totalBytes = 0;
    const QDir months(broadcastMonthsFolder());
    for (const QString& month : broadcastMonths()) {
        const QString path =
            months.filePath(QStringLiteral("lichess_db_broadcast_%1.pgn.zst").arg(month));
        paths.push_back(QFile::encodeName(path).toStdString());
        totalBytes += QFileInfo(path).size();
    }
    const QString folder = QDir(m_root).filePath(QString::fromLatin1(kBroadcastsSource));
    const QString part = folder + QStringLiteral(".part");
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    const QPointer<DatabaseManager> self(this);
    const auto post = [self, id](const QString& phase, double fraction, quint64 games) {
        QMetaObject::invokeMethod(
            self,
            [self, id, phase, fraction, games] {
                if (self) {
                    self->update(id, phase, fraction, games);
                }
            },
            Qt::QueuedConnection);
    };
    const auto end = [self, id](const QString& error) {
        QMetaObject::invokeMethod(
            self,
            [self, id, error] {
                if (self) {
                    self->m_broadcastUpdate = {};
                    self->finished(id, error);
                }
            },
            Qt::QueuedConnection);
    };
    const QString reading = tr("Reading games");
    const QString indexing = tr("Indexing");
    const QString noGames = tr("No games were found in Lichess's broadcasts.");
    const QString notInPlace = tr("The database couldn't be put in place.");
    const QFuture<void> future = QtConcurrent::run([=] {
        QDir(part).removeRecursively();
        QDir().mkpath(part);
        gamedb::DatabaseInfo info;
        info.name = kBroadcastsSource;
        info.source = kBroadcastsSource;
        auto writer = gamedb::DatabaseWriter::create(part.toStdString(), info);
        if (!writer.ok()) {
            QDir(part).removeRecursively();
            end(QString::fromStdString(writer.error().message));
            return;
        }
        std::atomic<std::uint64_t> bytes{0};
        gamedb::PgnFilesReader reader(paths, &bytes);
        QElapsedTimer sinceReport;
        sinceReport.start();
        gamedb::ImportOptions options;
        options.cancel = cancel.get();
        options.progress = [&](const gamedb::ImportProgress& counts) {
            if (sinceReport.elapsed() < 200) {
                return;
            }
            sinceReport.restart();
            post(reading,
                 static_cast<double>(bytes.load()) /
                     static_cast<double>(std::max<qint64>(totalBytes, 1)),
                 counts.games);
        };
        const gamedb::ImportResult result = gamedb::importPgn(
            **writer, [&](char* buffer, std::size_t size) { return reader.read(buffer, size); },
            options);
        const auto fail = [&](const QString& error) {
            writer->reset();
            QDir(part).removeRecursively();
            end(error);
        };
        if (result.cancelled) {
            fail(QStringLiteral("cancelled"));
            return;
        }
        if (!reader.error().empty()) {
            // A damaged month: removed, so the next update downloads it again.
            for (const std::string& path : paths) {
                if (reader.error().find(path) != std::string::npos) {
                    QFile::remove(QFile::decodeName(path.c_str()));
                }
            }
            fail(QString::fromStdString(reader.error()));
            return;
        }
        if (result.counts.games == 0) {
            fail(noGames);
            return;
        }
        const quint64 games = result.counts.games;
        const std::string error =
            (*writer)->finish([&](double done) { post(indexing, done, games); });
        writer->reset();
        if (!error.empty()) {
            QDir(part).removeRecursively();
            end(QString::fromStdString(error));
            return;
        }
        // The old one goes only once the new one is in place (open copies keep working:
        // their files stay until they are closed).
        const QString old = folder + QStringLiteral(".old");
        QDir(old).removeRecursively();
        if (QFileInfo::exists(folder)) {
            QDir().rename(folder, old); // NOLINT(readability-suspicious-call-argument)
        }
        if (!QDir().rename(part, folder)) {
            end(notInPlace);
            return;
        }
        QDir(old).removeRecursively();
        end({});
    });
    m_running.push_back({id, cancel, future});
}

} // namespace kiebitz::app
