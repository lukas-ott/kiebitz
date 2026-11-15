#include "ProfileStore.hpp"

#include "AccountFinder.hpp"
#include "kiebitz/gamedb/import.hpp"
#include "kiebitz/pgn/pgn.hpp"
#include "net/Lichess.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <deque>
#include <functional>
#include <tuple>
#include <unordered_set>

namespace kiebitz::app {

/// A sync in progress: the accounts still to download, then the build.
struct ProfileStore::Job {
    QString profileId;
    std::deque<QString> accounts;
};

namespace {

constexpr auto kOtb = "otb";
constexpr auto kAll = "all";

QString siteName(AccountSite site) {
    return site == AccountSite::Lichess ? QStringLiteral("lichess") : QStringLiteral("chesscom");
}

QJsonObject ratingsJson(const std::map<QString, int>& ratings) {
    QJsonObject object;
    for (const auto& [speed, rating] : ratings) {
        object[speed] = rating;
    }
    return object;
}

QJsonObject toJson(const FidePlayer& player) {
    return {{QStringLiteral("id"), player.id},
            {QStringLiteral("name"), player.name},
            {QStringLiteral("title"), player.title},
            {QStringLiteral("federation"), player.federation},
            {QStringLiteral("standard"), player.standard},
            {QStringLiteral("rapid"), player.rapid},
            {QStringLiteral("blitz"), player.blitz},
            {QStringLiteral("born"), player.born}};
}

FidePlayer fideFromJson(const QJsonObject& object) {
    FidePlayer player;
    player.id = object[QStringLiteral("id")].toInt();
    player.name = object[QStringLiteral("name")].toString();
    player.title = object[QStringLiteral("title")].toString();
    player.federation = object[QStringLiteral("federation")].toString();
    player.standard = object[QStringLiteral("standard")].toInt();
    player.rapid = object[QStringLiteral("rapid")].toInt();
    player.blitz = object[QStringLiteral("blitz")].toInt();
    player.born = object[QStringLiteral("born")].toInt();
    return player;
}

/// FIDE is searched for names of two words or more ("Me" or "Magnus" alone would find anyone).
bool worthLookingUp(const QString& name) {
    return name.split(u' ', Qt::SkipEmptyParts).size() >= 2;
}

/// Why a download failed, in words.
QString downloadError(const QString& site, const QString& name, int status, const QString& error) {
    if (status == 404) {
        return ProfileStore::tr("There is no %1 account called %2.").arg(site, name);
    }
    if (status == 429) {
        return ProfileStore::tr("%1 asks to wait a minute before the next download.").arg(site);
    }
    return ProfileStore::tr("%1's %2 games couldn't be downloaded: %3").arg(name, site, error);
}

std::string readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
}

/// What makes a game in a PGN text the same game (pgn::gameKey); empty if it can't be read.
std::string keyOf(std::string_view text) {
    const auto game = pgn::parseGame(text);
    return game.ok() ? pgn::gameKey(*game) : std::string();
}

/// The PGN files of a folder, oldest name first.
QStringList pgnFiles(const QString& folder) {
    QStringList files;
    const auto entries =
        QDir(folder).entryInfoList({QStringLiteral("*.pgn")}, QDir::Files, QDir::Name);
    for (const QFileInfo& entry : entries) {
        files << entry.absoluteFilePath();
    }
    return files;
}

/// Builds a database from PGN files into `folder` (replacing what is there). Empty on
/// success, else the error.
QString buildDatabase(const QStringList& files, const QString& folder, const QString& name) {
    const QString part = folder + QStringLiteral(".part");
    QDir(part).removeRecursively();
    QDir().mkpath(part);
    gamedb::DatabaseInfo info;
    info.name = name.toStdString();
    info.source = "profile";
    auto writer = gamedb::DatabaseWriter::create(part.toStdString(), info);
    if (!writer.ok()) {
        return QString::fromStdString(writer.error().message);
    }
    // The files one after another, as one stream.
    int index = 0;
    QFile file;
    const auto read = [&](char* buffer, std::size_t size) -> std::size_t {
        while (true) {
            if (!file.isOpen()) {
                if (index >= files.size()) {
                    return 0;
                }
                file.setFileName(files[index++]);
                if (!file.open(QIODevice::ReadOnly)) {
                    continue;
                }
                buffer[0] = '\n'; // a game never runs from one file into the next
                return 1;
            }
            const qint64 got = file.read(buffer, static_cast<qint64>(size));
            if (got > 0) {
                return static_cast<std::size_t>(got);
            }
            file.close();
        }
    };
    gamedb::importPgn(**writer, read);
    if (const std::string error = (*writer)->finish(); !error.empty()) {
        return QString::fromStdString(error);
    }
    writer->reset();
    // Swap: the old one goes only once the new one is in place.
    const QString old = folder + QStringLiteral(".old");
    QDir(old).removeRecursively();
    if (QFileInfo::exists(folder)) {
        QDir().rename(folder, old); // NOLINT(readability-suspicious-call-argument): moved aside
    }
    if (!QDir().rename(part, folder)) {
        return QStringLiteral("The database couldn't be put in place.");
    }
    QDir(old).removeRecursively();
    return {};
}

/// PGN files' games as one file, each game once (the same game from your PGN, Lichess's
/// broadcasts and Caissabase, written differently: pgn::sameGameKey). Whether it was written.
bool writeOnce(const QStringList& files, const QString& target) {
    std::unordered_set<std::string> seen;
    std::string once;
    for (const QString& file : files) {
        const std::string text = readAll(file);
        for (const pgn::GameEntry& entry : pgn::indexGames(text)) {
            const std::string_view game = std::string_view(text).substr(entry.offset, entry.length);
            const auto parsed = pgn::parseGame(game);
            if (parsed.ok() && !seen.insert(pgn::sameGameKey(*parsed)).second) {
                continue;
            }
            once.append(game);
            once += "\n\n";
        }
    }
    QDir().mkpath(QFileInfo(target).path());
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    out.write(once.data(), static_cast<qint64>(once.size()));
    return true;
}

/// The cross-source keys (pgn::sameGameKey) of a PGN file's games.
void addSameGameKeys(const QString& file, std::unordered_set<std::string>& keys) {
    const std::string text = readAll(file);
    for (const pgn::GameEntry& entry : pgn::indexGames(text)) {
        const auto game = pgn::parseGame(std::string_view(text).substr(entry.offset, entry.length));
        if (game.ok()) {
            keys.insert(pgn::sameGameKey(*game));
        }
    }
}

/// A database's part of a profile: "src-lichess-broadcasts" for "Lichess broadcasts".
constexpr auto kSourcePrefix = "src-";
QString sourcePart(const QString& database) {
    QString slug;
    for (const QChar c : database.toLower()) {
        slug += c.isLetterOrNumber() ? c : QChar(u'-');
    }
    return QString::fromLatin1(kSourcePrefix) + slug;
}

/// The database parts a profile's folder has, by folder name.
QStringList sourceParts(const QString& profileFolder) {
    return QDir(profileFolder)
        .entryList({QString::fromLatin1(kSourcePrefix) + u'*'}, QDir::Dirs | QDir::NoDotAndDotDot,
                   QDir::Name);
}

/// Games the user deleted from a database's games: not brought back by the next look.
QString removedFile(const QString& profileFolder) {
    return QDir(profileFolder).filePath(QStringLiteral("removed-fide-games.txt"));
}

} // namespace

ProfileStore::ProfileStore(const QString& root, QObject* parent)
    : QObject(parent), m_root(root.isEmpty() ? defaultRoot() : root),
      m_network(new QNetworkAccessManager(this)) {
    m_finder = new AccountFinder(*m_network, this);
    load();
}

ProfileStore::~ProfileStore() {
    wait();
}

QString ProfileStore::defaultRoot() {
    QString folder = qEnvironmentVariable("KIEBITZ_SETTINGS_DIR"); // tests: never the real one
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    return QDir(folder).filePath(QStringLiteral("Profiles"));
}

// ---- Profiles and accounts ---------------------------------------------------------------------

void ProfileStore::load() {
    QFile file(QDir(m_root).filePath(QStringLiteral("profiles.json")));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonArray profiles = QJsonDocument::fromJson(file.readAll()).array();
        for (const auto& value : profiles) {
            const QJsonObject object = value.toObject();
            Profile profile;
            profile.id = object[QStringLiteral("id")].toString();
            profile.name = object[QStringLiteral("name")].toString();
            profile.main = object[QStringLiteral("main")].toBool();
            for (const auto& accountValue : object[QStringLiteral("accounts")].toArray()) {
                const QJsonObject a = accountValue.toObject();
                Account account;
                account.id = a[QStringLiteral("id")].toString();
                account.site = a[QStringLiteral("site")].toString() == u"chesscom"
                                   ? AccountSite::ChessCom
                                   : AccountSite::Lichess;
                account.username = a[QStringLiteral("username")].toString();
                account.synced =
                    QDateTime::fromString(a[QStringLiteral("synced")].toString(), Qt::ISODate);
                account.lichessSince =
                    static_cast<qint64>(a[QStringLiteral("lichessSince")].toDouble());
                account.lichessClocks = a[QStringLiteral("lichessClocks")].toBool();
                for (const auto& month : a[QStringLiteral("chessComMonths")].toArray()) {
                    account.chessComMonths << month.toString();
                }
                account.title = a[QStringLiteral("title")].toString();
                account.siteGames = a[QStringLiteral("siteGames")].toInt();
                const QJsonObject changes = a[QStringLiteral("ratingChanges")].toObject();
                for (auto it = changes.begin(); it != changes.end(); ++it) {
                    account.ratingChanges[it.key()] = it.value().toInt();
                }
                const QJsonObject ratings = a[QStringLiteral("ratings")].toObject();
                for (auto it = ratings.begin(); it != ratings.end(); ++it) {
                    account.ratings[it.key()] = it.value().toInt();
                }
                profile.accounts.push_back(account);
            }
            if (object.contains(QStringLiteral("fide"))) {
                profile.fide = fideFromJson(object[QStringLiteral("fide")].toObject());
            }
            for (const auto& candidate : object[QStringLiteral("fideCandidates")].toArray()) {
                profile.fideCandidates.push_back(fideFromJson(candidate.toObject()));
            }
            profile.fideChosen = object[QStringLiteral("fideChosen")].toBool();
            profile.fideLooked = object[QStringLiteral("fideLooked")].toBool();
            for (const auto& entry : object[QStringLiteral("suggestions")].toArray()) {
                const QJsonObject s = entry.toObject();
                SuggestedAccount suggestion;
                suggestion.site = s[QStringLiteral("site")].toString() == u"chesscom"
                                      ? AccountSite::ChessCom
                                      : AccountSite::Lichess;
                suggestion.username = s[QStringLiteral("username")].toString();
                for (const auto& reason : s[QStringLiteral("reasons")].toArray()) {
                    suggestion.reasons << reason.toString();
                }
                profile.suggestions.push_back(suggestion);
            }
            profile.suggestionsFor = object[QStringLiteral("suggestionsFor")].toInt();
            for (const auto& name : object[QStringLiteral("dismissed")].toArray()) {
                profile.dismissed << name.toString();
            }
            const QJsonObject wiki = object[QStringLiteral("wiki")].toObject();
            profile.wiki.item = wiki[QStringLiteral("item")].toString();
            for (const auto& name : wiki[QStringLiteral("lichess")].toArray()) {
                profile.wiki.lichess << name.toString();
            }
            profile.wiki.photo = wiki[QStringLiteral("photo")].toString();
            const QJsonObject articles = wiki[QStringLiteral("articles")].toObject();
            for (auto it = articles.begin(); it != articles.end(); ++it) {
                profile.wiki.articles.emplace(it.key(), it.value().toString());
            }
            if (!profile.id.isEmpty()) {
                m_profiles.push_back(std::move(profile));
            }
        }
    }
    // There is always a main profile: you.
    if (std::none_of(m_profiles.begin(), m_profiles.end(),
                     [](const Profile& p) { return p.main; })) {
        Profile you;
        you.id = QStringLiteral("me");
        you.name = tr("Me");
        you.main = true;
        m_profiles.insert(m_profiles.begin(), std::move(you));
        save();
    }
    for (const Profile& profile : m_profiles) {
        openDatabases(profile.id);
    }
    // Names not looked up yet (FIDE couldn't be reached last time): try now.
    QStringList pending;
    for (const Profile& profile : m_profiles) {
        if (!profile.fideLooked && !profile.fideChosen) {
            pending << profile.id;
        }
    }
    QMetaObject::invokeMethod(
        this,
        [this, pending] {
            for (const QString& id : pending) {
                lookUpFide(id);
            }
        },
        Qt::QueuedConnection);
}

void ProfileStore::save() const {
    QJsonArray profiles;
    for (const Profile& profile : m_profiles) {
        QJsonArray accounts;
        for (const Account& account : profile.accounts) {
            accounts.append(QJsonObject{
                {QStringLiteral("id"), account.id},
                {QStringLiteral("site"), siteName(account.site)},
                {QStringLiteral("username"), account.username},
                {QStringLiteral("synced"), account.synced.toString(Qt::ISODate)},
                {QStringLiteral("lichessSince"), static_cast<double>(account.lichessSince)},
                {QStringLiteral("lichessClocks"), account.lichessClocks},
                {QStringLiteral("chessComMonths"),
                 QJsonArray::fromStringList(account.chessComMonths)},
                {QStringLiteral("title"), account.title},
                {QStringLiteral("ratings"), ratingsJson(account.ratings)},
                {QStringLiteral("ratingChanges"), ratingsJson(account.ratingChanges)},
                {QStringLiteral("siteGames"), account.siteGames},
            });
        }
        QJsonObject object{{QStringLiteral("id"), profile.id},
                           {QStringLiteral("name"), profile.name},
                           {QStringLiteral("main"), profile.main},
                           {QStringLiteral("accounts"), accounts},
                           {QStringLiteral("fideChosen"), profile.fideChosen},
                           {QStringLiteral("fideLooked"), profile.fideLooked}};
        if (profile.fide.has_value()) {
            object[QStringLiteral("fide")] = toJson(profile.fide.value_or(FidePlayer{}));
        }
        QJsonArray candidates;
        for (const FidePlayer& candidate : profile.fideCandidates) {
            candidates.append(toJson(candidate));
        }
        object[QStringLiteral("fideCandidates")] = candidates;
        QJsonArray suggestions;
        for (const SuggestedAccount& suggestion : profile.suggestions) {
            suggestions.append(QJsonObject{
                {QStringLiteral("site"), siteName(suggestion.site)},
                {QStringLiteral("username"), suggestion.username},
                {QStringLiteral("reasons"), QJsonArray::fromStringList(suggestion.reasons)}});
        }
        object[QStringLiteral("suggestions")] = suggestions;
        object[QStringLiteral("suggestionsFor")] = profile.suggestionsFor;
        object[QStringLiteral("dismissed")] = QJsonArray::fromStringList(profile.dismissed);
        if (!profile.wiki.item.isEmpty()) {
            QJsonObject articles;
            for (const auto& [language, url] : profile.wiki.articles) {
                articles[language] = url;
            }
            object[QStringLiteral("wiki")] = QJsonObject{
                {QStringLiteral("item"), profile.wiki.item},
                {QStringLiteral("lichess"), QJsonArray::fromStringList(profile.wiki.lichess)},
                {QStringLiteral("photo"), profile.wiki.photo},
                {QStringLiteral("articles"), articles}};
        }
        profiles.append(object);
    }
    QDir().mkpath(m_root);
    // Written next to it and renamed, so a crash never leaves half a file.
    const QString path = QDir(m_root).filePath(QStringLiteral("profiles.json"));
    QFile file(path + QStringLiteral(".part"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(profiles).toJson());
        file.close();
        QFile::remove(path);
        QFile::rename(path + QStringLiteral(".part"), path);
    }
}

Profile* ProfileStore::find(const QString& id) {
    for (Profile& profile : m_profiles) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const Profile* ProfileStore::profile(const QString& id) const {
    for (const Profile& p : m_profiles) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

QString ProfileStore::folderOf(const QString& profileId) const {
    return QDir(m_root).filePath(profileId);
}

QString ProfileStore::addProfile(const QString& name) {
    QString id;
    for (int n = 1; id.isEmpty() || find(id) != nullptr; ++n) {
        id = QStringLiteral("profile-%1").arg(n);
    }
    Profile profile;
    profile.id = id;
    profile.name = name.trimmed().isEmpty() ? tr("New profile") : name.trimmed();
    m_profiles.push_back(std::move(profile));
    save();
    emit changed();
    lookUpFide(id);
    return id;
}

void ProfileStore::renameProfile(const QString& id, const QString& name) {
    if (Profile* profile = find(id); profile != nullptr && !name.trimmed().isEmpty()) {
        profile->name = name.trimmed();
        // Another name: another person, perhaps.
        profile->fide.reset();
        profile->fideCandidates.clear();
        profile->fideChosen = false;
        profile->fideLooked = false;
        save();
        emit changed();
        lookUpFide(id);
    }
}

void ProfileStore::removeProfile(const QString& id) {
    const Profile* profile = find(id);
    if (profile == nullptr || profile->main || busy(id)) {
        return;
    }
    std::erase_if(m_profiles, [&](const Profile& p) { return p.id == id; });
    save();
    QDir(folderOf(id)).removeRecursively();
    emit changed();
}

QString ProfileStore::addAccount(const QString& profileId, AccountSite site,
                                 const QString& username) {
    Profile* profile = find(profileId);
    const QString name = username.trimmed();
    if (profile == nullptr || name.isEmpty()) {
        return {};
    }
    QString id = siteName(site) + u'-' + name.toLower();
    if (std::any_of(profile->accounts.begin(), profile->accounts.end(),
                    [&](const Account& a) { return a.id == id; })) {
        return {};
    }
    Account account;
    account.id = id;
    account.site = site;
    account.username = name;
    profile->accounts.push_back(account);
    std::erase_if(profile->suggestions, [&](const SuggestedAccount& suggestion) {
        return suggestion.site == site &&
               suggestion.username.compare(name, Qt::CaseInsensitive) == 0;
    });
    save();
    emit changed();
    sync(profileId);
    return id;
}

void ProfileStore::removeAccount(const QString& profileId, const QString& accountId) {
    Profile* profile = find(profileId);
    if (profile == nullptr || busy(profileId)) {
        return;
    }
    std::erase_if(profile->accounts, [&](const Account& a) { return a.id == accountId; });
    save();
    QDir(QDir(folderOf(profileId)).filePath(accountId)).removeRecursively();
    QDir(QDir(folderOf(profileId)).filePath(QStringLiteral("db/") + accountId)).removeRecursively();
    profile->parts.erase(accountId);
    emit changed();
    build(profileId);
}

void ProfileStore::addOtbGames(const QString& profileId, const QStringList& files) {
    if (find(profileId) == nullptr || busy(profileId) || files.isEmpty()) {
        return;
    }
    setStatus(profileId, tr("Adding games…"));
    const QString otb = QDir(folderOf(profileId)).filePath(QString::fromLatin1(kOtb));
    // Only the games not there yet (nor twice in what's added): each file's new games are
    // copied into a file of their own.
    const auto future = QtConcurrent::run([files, otb] {
        QDir().mkpath(otb);
        // The same game twice is left out, also when it comes from another source with the
        // names written differently (ChessBase and Lichess's broadcasts): sameGameKey.
        std::unordered_set<std::string> seen;
        for (const QString& file : pgnFiles(otb)) {
            addSameGameKeys(file, seen);
        }
        std::pair<int, int> counts{0, 0}; // added, skipped
        for (const QString& file : files) {
            const std::string text = readAll(file);
            std::string fresh;
            for (const pgn::GameEntry& entry : pgn::indexGames(text)) {
                const std::string_view game =
                    std::string_view(text).substr(entry.offset, entry.length);
                const auto parsed = pgn::parseGame(game);
                const std::string key = parsed.ok() ? pgn::sameGameKey(*parsed) : std::string();
                if (!key.empty() && !seen.insert(key).second) {
                    ++counts.second; // already there
                    continue;
                }
                fresh.append(game);
                fresh += "\n\n";
                ++counts.first;
            }
            if (fresh.empty()) {
                continue;
            }
            const QFileInfo info(file);
            QString target = QDir(otb).filePath(info.completeBaseName() + QStringLiteral(".pgn"));
            for (int n = 2; QFileInfo::exists(target); ++n) {
                target = QDir(otb).filePath(
                    QStringLiteral("%1 %2.pgn").arg(info.completeBaseName()).arg(n));
            }
            QFile out(target);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(fresh.data(), static_cast<qint64>(fresh.size()));
            }
        }
        return counts;
    });
    auto* watcher = new QFutureWatcher<std::pair<int, int>>(
        this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<std::pair<int, int>>::finished, this,
            [this, watcher, profileId] {
                watcher->deleteLater();
                const auto [added, skipped] = watcher->result();
                emit otbAdded(profileId, added, skipped);
                build(profileId);
            });
    watcher->setFuture(future);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void ProfileStore::findAccounts(const QString& profileId, bool again) {
    Profile* profile = find(profileId);
    if (profile == nullptr || profile->main || !profile->fide || profile->suggesting ||
        (!again && profile->suggestionsFor == profile->fide->id)) {
        return;
    }
    profile->suggesting = true;
    emit changed();
    const int fideId = profile->fide->id;
    m_finder->find(*profile->fide, [this, profileId,
                                    fideId](const std::vector<AccountSuggestion>& found,
                                            const WikiPerson& wiki) {
        Profile* done = find(profileId);
        if (done == nullptr) {
            return;
        }
        done->suggesting = false;
        if (!done->fide || done->fide->id != fideId) {
            emit changed(); // another player meanwhile: looked for again by its change
            return;
        }
        done->suggestions.clear();
        for (const AccountSuggestion& suggestion : found) {
            const QString key = siteName(suggestion.site) + u':' + suggestion.username.toLower();
            const bool added = std::any_of(
                done->accounts.begin(), done->accounts.end(), [&](const Account& account) {
                    return account.site == suggestion.site &&
                           account.username.compare(suggestion.username, Qt::CaseInsensitive) == 0;
                });
            if (!added && !done->dismissed.contains(key)) {
                done->suggestions.push_back(
                    {suggestion.site, suggestion.username, suggestion.reasons});
            }
        }
        done->suggestionsFor = fideId;
        setWiki(profileId, wiki);
        save();
        emit changed();
    });
}

void ProfileStore::dismissSuggestion(const QString& profileId, AccountSite site,
                                     const QString& username) {
    Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    profile->dismissed << siteName(site) + u':' + username.toLower();
    std::erase_if(profile->suggestions, [&](const SuggestedAccount& suggestion) {
        return suggestion.site == site &&
               suggestion.username.compare(username, Qt::CaseInsensitive) == 0;
    });
    save();
    emit changed();
}

bool ProfileStore::hasGameSources() const {
    return m_gameSources && !m_gameSources().empty();
}

void ProfileStore::refreshSourceGames(const QString& profileId) {
    const Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    if (busy(profileId)) {
        m_sourcesPending.insert(profileId);
        return;
    }
    setStatus(profileId, tr("Looking for tournament games…"));
    collectSourceGames(profileId, [this, profileId](bool changed) {
        if (changed) {
            build(profileId);
        } else {
            setStatus(profileId, {});
        }
    });
}

void ProfileStore::collectSourceGames(const QString& profileId,
                                      std::function<void(bool changed)> done) {
    const Profile* profile = find(profileId);
    if (profile == nullptr) {
        done(false);
        return;
    }
    // The player: their FIDE id, and the ways their name is written over the board. Without
    // a FIDE player, nothing is looked for: a name alone finds namesakes too.
    const std::uint32_t fideId = profile->fide ? static_cast<std::uint32_t>(profile->fide->id) : 0;
    std::vector<std::string> names;
    for (const QString& name : namesOf(*profile, QString::fromLatin1(kOtb))) {
        // Full names only: "Carlsen,M" in a big database is any M. Carlsen.
        const QString first = name.section(u',', 1).remove(u'.').trimmed();
        if (!name.contains(u',') || first.size() > 1) {
            names.push_back(name.toStdString());
        }
    }
    std::vector<std::pair<QString, std::shared_ptr<const gamedb::Database>>> sources;
    if (m_gameSources && fideId != 0) {
        sources = m_gameSources();
    }
    auto* watcher =
        new QFutureWatcher<bool>(this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<bool>::finished, this, [watcher, done = std::move(done)] {
        watcher->deleteLater();
        done(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([folder = folderOf(profileId), fideId, names, sources] {
        bool changed = false;
        // From before each database was a source of its own: the broadcasts' file among the
        // over-the-board games.
        const QString old = QDir(folder).filePath(QStringLiteral("otb/Lichess broadcasts.pgn"));
        if (QFile::exists(old)) {
            QFile::remove(old);
            changed = true;
        }
        // Games deleted from a source: not brought back.
        std::unordered_set<std::string> removed;
        QFile removedList(removedFile(folder));
        if (removedList.open(QIODevice::ReadOnly)) {
            for (const QByteArray& line : removedList.readAll().split('\n')) {
                if (!line.isEmpty()) {
                    removed.insert(line.toStdString());
                }
            }
        }
        QStringList kept;
        for (const auto& [name, db] : sources) {
            // By FIDE id where the database has them (exactly them); else by name.
            const std::vector<gamedb::GameId> ids =
                db->hasFideIds() ? db->gamesOfFideId(fideId) : db->gamesOfPlayer(names);
            std::string text;
            std::unordered_set<std::string> seen;
            for (const gamedb::GameId id : ids) {
                const auto game = db->game(id);
                if (!game) {
                    continue;
                }
                const std::string key = pgn::sameGameKey(*game);
                if (removed.contains(key) || !seen.insert(key).second) {
                    continue;
                }
                text += pgn::writeGame(*game);
                text += '\n';
            }
            const QString part = QDir(folder).filePath(sourcePart(name));
            const QString file = QDir(part).filePath(QStringLiteral("games.pgn"));
            if (text.empty()) {
                continue; // none there: the part goes (below)
            }
            kept << QFileInfo(part).fileName();
            if (readAll(file) == text) {
                continue;
            }
            QDir().mkpath(part);
            QFile out(file);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(text.data(), static_cast<qint64>(text.size()));
            }
            QFile label(QDir(part).filePath(QStringLiteral("name.txt")));
            if (label.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                label.write(name.toUtf8());
            }
            changed = true;
        }
        // Sources with no games of theirs (any more), or gone: their parts go.
        for (const QString& part : sourceParts(folder)) {
            if (!kept.contains(part)) {
                QDir(QDir(folder).filePath(part)).removeRecursively();
                changed = true;
            }
        }
        return changed;
    }));
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void ProfileStore::checkAccount(
    AccountSite site, const QString& username,
    std::function<void(const QString& name, const QString& error)> done) {
    // Lichess: /api/user/<name>; chess.com: /pub/player/<name>. A 404: no such account.
    const QString user = username.trimmed();
    if (site == AccountSite::Lichess && lichess::pauseLeftMs() > 0) {
        done({}, downloadError(QStringLiteral("Lichess"), user, 429, QString()));
        return;
    }
    QNetworkRequest request(
        site == AccountSite::Lichess
            ? m_lichess.resolved(QUrl(QStringLiteral("/api/user/") + user))
            : m_chessCom.resolved(QUrl(QStringLiteral("/pub/player/") + user.toLower())));
    request.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = m_network->get(request);
    const QString siteName =
        site == AccountSite::Lichess ? QStringLiteral("Lichess") : QStringLiteral("chess.com");
    connect(reply, &QNetworkReply::finished, this,
            [reply, site, user, siteName, done = std::move(done)] {
                reply->deleteLater();
                // From Lichess, a 429 starts the minute's pause.
                const int status =
                    site == AccountSite::Lichess
                        ? lichess::status(reply)
                        : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 404) {
                    done({}, {});
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status != 200) {
                    done({}, tr("Couldn't reach %1 to look up %2. Try again in a moment.")
                                 .arg(siteName, user));
                    return;
                }
                const QJsonObject account = QJsonDocument::fromJson(reply->readAll()).object();
                if (account.isEmpty() || account[QStringLiteral("closed")].toBool() ||
                    account[QStringLiteral("disabled")].toBool()) {
                    done({}, {}); // closed accounts have no games to follow
                    return;
                }
                // Lichess writes the name as its owner does ("CatalanSimp"); chess.com in lower
                // case, so there the name as typed stays.
                const QString name = site == AccountSite::Lichess
                                         ? account[QStringLiteral("username")].toString(user)
                                         : user;
                done(name, {});
            });
}

QString ProfileStore::databaseStamp(const QString& profileId, const QString& part) const {
    // The database's moves file: its size and when it was written. Several parts: theirs.
    const QStringList parts = splitParts(part);
    if (parts.size() > 1) {
        QStringList stamps;
        for (const QString& one : parts) {
            stamps << databaseStamp(profileId, one);
        }
        return stamps.join(u',');
    }
    const QFileInfo moves(
        QDir(folderOf(profileId))
            .filePath(QStringLiteral("db/%1/moves.bin")
                          .arg(part.isEmpty() ? QString::fromLatin1(kAll) : part)));
    return moves.exists() ? QStringLiteral("%1-%2")
                                .arg(moves.size())
                                .arg(moves.lastModified().toMSecsSinceEpoch())
                          : QString();
}

std::shared_ptr<const gamedb::Database> ProfileStore::database(const QString& profileId,
                                                               const QString& part) const {
    const Profile* profile = this->profile(profileId);
    if (profile == nullptr) {
        return nullptr;
    }
    const QStringList parts = splitParts(part);
    if (parts.isEmpty()) {
        return profile->all;
    }
    if (parts.size() == 1) {
        const auto found = profile->parts.find(parts.front());
        return found != profile->parts.end() ? found->second : nullptr;
    }
    const auto found = m_combined.find(profileId + u'|' + partsKey(parts));
    return found != m_combined.end() ? found->second : nullptr;
}

void ProfileStore::combine(const QString& profileId, const QString& part) {
    const QStringList parts = splitParts(part);
    const QString key = profileId + u'|' + partsKey(parts);
    if (parts.size() < 2 || m_combined.contains(key) || m_combining.contains(key) ||
        profile(profileId) == nullptr) {
        return;
    }
    // Their games in one database, each game once (the same game in your PGN, Lichess's
    // broadcasts and Caissabase), built in the background and kept until the games change.
    m_combining.insert(key);
    QStringList files;
    for (const QString& folder : partFolders(*profile(profileId), partsKey(parts))) {
        files << pgnFiles(folder);
    }
    const QString folder =
        QDir(folderOf(profileId))
            .filePath(
                QStringLiteral("db/sel-%1")
                    .arg(QString::fromLatin1(
                        QCryptographicHash::hash(partsKey(parts).toUtf8(), QCryptographicHash::Sha1)
                            .toHex()
                            .left(12))));
    const QString name = profile(profileId)->name;
    using Opened = std::shared_ptr<const gamedb::Database>;
    auto* watcher =
        new QFutureWatcher<Opened>(this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<Opened>::finished, this, [this, watcher, key] {
        watcher->deleteLater();
        m_combining.remove(key);
        if (Opened opened = watcher->result()) {
            m_combined[key] = std::move(opened);
            emit changed();
        }
    });
    watcher->setFuture(QtConcurrent::run([files, folder, name]() -> Opened {
        const QString once = folder + QStringLiteral(".pgn");
        writeOnce(files, once);
        if (!buildDatabase({once}, folder, name).isEmpty()) {
            return nullptr;
        }
        auto opened = gamedb::Database::open(folder.toStdString());
        return opened.ok() ? Opened(std::move(*opened)) : nullptr;
    }));
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

QString ProfileStore::cacheFile(const QString& profileId, const QString& name) const {
    const QString folder = QDir(folderOf(profileId)).filePath(QStringLiteral("cache"));
    QDir().mkpath(folder);
    return QDir(folder).filePath(name);
}

QStringList ProfileStore::partFolders(const Profile& profile, const QString& part) const {
    QStringList parts;
    if (part.isEmpty()) {
        parts << QString::fromLatin1(kOtb);
        for (const Account& account : profile.accounts) {
            parts << account.id;
        }
        parts << sourceParts(folderOf(profile.id));
    } else {
        parts << splitParts(part); // one part, or several ("otb+src-caissabase")
    }
    QStringList folders;
    for (const QString& name : parts) {
        folders << QDir(folderOf(profile.id)).filePath(name);
    }
    return folders;
}

void ProfileStore::originalGame(const QString& profileId, const QString& part,
                                const core::Game& game,
                                std::function<void(const core::Game&)> done) {
    const Profile* profile = find(profileId);
    if (profile == nullptr) {
        done(game);
        return;
    }
    // Owned by the store and deleted when done (deleteLater); the analyzer can't see that.
    auto* watcher = new QFutureWatcher<std::optional<core::Game>>(
        this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<std::optional<core::Game>>::finished, this,
            // NOLINTNEXTLINE(bugprone-exception-escape): `done` opens a tab, as any slot does
            [watcher, game, done = std::move(done)] {
                watcher->deleteLater();
                const std::optional<core::Game> found = watcher->result();
                done(found ? *found : game);
            });
    watcher->setFuture(QtConcurrent::run([folders = partFolders(*profile, part),
                                          key = pgn::gameKey(game)]() -> std::optional<core::Game> {
        for (const QString& folder : folders) {
            for (const QString& file : pgnFiles(folder)) {
                const std::string text = readAll(file);
                for (const pgn::GameEntry& entry : pgn::indexGames(text)) {
                    auto parsed =
                        pgn::parseGame(std::string_view(text).substr(entry.offset, entry.length));
                    if (parsed.ok() && pgn::gameKey(*parsed) == key) {
                        return std::move(*parsed);
                    }
                }
            }
        }
        return std::nullopt;
    }));
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void ProfileStore::deleteGame(const QString& profileId, const QString& part,
                              const core::Game& game) {
    const Profile* profile = find(profileId);
    if (profile == nullptr || busy(profileId)) {
        return;
    }
    setStatus(profileId, tr("Deleting the game…"));
    const auto future = QtConcurrent::run([folders = partFolders(*profile, part),
                                           key = pgn::gameKey(game),
                                           sameKey = pgn::sameGameKey(game),
                                           removed = removedFile(folderOf(profileId))] {
        // The first copy of the game, taken out of its file (the file goes if it's left empty).
        for (const QString& folder : folders) {
            for (const QString& file : pgnFiles(folder)) {
                std::string text = readAll(file);
                const auto entries = pgn::indexGames(text);
                for (std::size_t k = 0; k < entries.size(); ++k) {
                    const pgn::GameEntry& entry = entries[k];
                    if (keyOf(std::string_view(text).substr(entry.offset, entry.length)) != key) {
                        continue;
                    }
                    // From a database's games: not brought back by the next look.
                    if (QFileInfo(file).dir().dirName().startsWith(
                            QString::fromLatin1(kSourcePrefix))) {
                        QFile list(removed);
                        if (list.open(QIODevice::Append)) {
                            list.write(QByteArray::fromStdString(sameKey) + '\n');
                        }
                    }
                    // Up to the next game (the blank lines after it go too).
                    const std::size_t end =
                        k + 1 < entries.size() ? entries[k + 1].offset : text.size();
                    text.erase(entry.offset, end - entry.offset);
                    if (entries.size() == 1) {
                        QFile::remove(file);
                    } else {
                        QFile out(file);
                        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            out.write(text.data(), static_cast<qint64>(text.size()));
                        }
                    }
                    return true;
                }
            }
        }
        return false;
    });
    auto* watcher =
        new QFutureWatcher<bool>(this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, profileId] {
        watcher->deleteLater();
        if (!watcher->result()) {
            setStatus(profileId, {});
            emit failed(profileId, tr("That game wasn't found in the profile's files."));
            return;
        }
        build(profileId);
    });
    watcher->setFuture(future);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

QString ProfileStore::status(const QString& profileId) const {
    const auto found = m_status.find(profileId);
    return found != m_status.end() ? found->second : QString();
}

bool ProfileStore::busy(const QString& profileId) const {
    return !status(profileId).isEmpty();
}

void ProfileStore::setStatus(const QString& profileId, const QString& status) {
    if (status.isEmpty()) {
        m_status.erase(profileId);
    } else {
        m_status[profileId] = status;
    }
    emit progress(profileId);
    if (status.isEmpty() && m_sourcesPending.remove(profileId)) {
        refreshSourceGames(profileId); // asked for while busy
    }
}

void ProfileStore::setServers(const QUrl& lichess, const QUrl& chessCom, const QUrl& fide,
                              const QUrl& wiki) {
    m_lichess = lichess;
    m_chessCom = chessCom;
    if (fide.isValid()) {
        m_fide = fide;
    }
    const QUrl wikis = wiki.isValid() ? wiki : fide;
    if (wikis.isValid()) {
        m_commons = wikis;
    }
    m_finder->setServers(lichess, chessCom,
                         wikis.isValid() ? wikis
                                         : QUrl(QStringLiteral("https://query.wikidata.org")));
}

QString ProfileStore::photoFile(const QString& profileId) const {
    const QString path = QDir(folderOf(profileId)).filePath(QStringLiteral("photo.jpg"));
    return QFile::exists(path) ? path : QString();
}

void ProfileStore::setWiki(const QString& profileId, const WikiPerson& wiki) {
    Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    const QString path = QDir(folderOf(profileId)).filePath(QStringLiteral("photo.jpg"));
    const bool newPhoto = wiki.photo != profile->wiki.photo || !QFile::exists(path);
    profile->wiki = wiki;
    if (wiki.photo.isEmpty()) {
        QFile::remove(path);
        return;
    }
    if (!newPhoto) {
        return;
    }
    // A small one, as Commons makes it (it follows a few redirects to the file).
    QNetworkRequest request(
        m_commons.resolved(QUrl(commonsPhotoPath(wiki.photo, 160), QUrl::StrictMode)));
    request.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, profileId, path, photo = wiki.photo] {
                reply->deleteLater();
                const Profile* done = find(profileId);
                if (done == nullptr || done->wiki.photo != photo ||
                    reply->error() != QNetworkReply::NoError) {
                    return; // tried again with the next look for accounts
                }
                const QByteArray image = reply->readAll();
                if (image.isEmpty() || image.size() > 2'000'000) {
                    return;
                }
                QDir().mkpath(folderOf(profileId));
                QFile file(path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(image);
                    file.close();
                    emit changed();
                }
            });
}

// ---- FIDE --------------------------------------------------------------------------------------

void ProfileStore::lookUpFide(const QString& profileId) {
    const Profile* profile = find(profileId);
    if (profile == nullptr || !worthLookingUp(profile->name)) {
        return;
    }
    // FIDE's rating list as Lichess keeps it (FIDE's own site, ratings.fide.com, is often away):
    // everyone whose name fits, roughly; ranked here by how well.
    QUrl url = m_fide.resolved(QUrl(QStringLiteral("/api/fide/player")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), profile->name);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = m_network->get(request);
    const QString name = profile->name;
    if (Profile* looking = find(profileId)) {
        looking->fideLooking = true;
        looking->fideError.clear();
        emit changed();
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, profileId, name] {
        reply->deleteLater();
        Profile* found = find(profileId);
        if (found == nullptr || found->name != name) {
            return; // renamed meanwhile: that name's lookup is under way
        }
        found->fideLooking = false;
        if (reply->error() != QNetworkReply::NoError) {
            // Tried again on the next start, or with "Try Again".
            found->fideError = tr("Couldn't reach FIDE's rating list (on Lichess) right now.");
            emit changed();
            return;
        }
        std::vector<FidePlayer> matches = rankFideMatches(parseFideSearch(reply->readAll()), name);
        if (matches.size() > 8) {
            matches.resize(8);
        }
        found->fideCandidates = matches;
        found->fideLooked = true;
        const int before = found->fide ? found->fide->id : 0;
        if (!found->fideChosen) {
            found->fide = matches.empty() ? std::nullopt : std::optional(matches.front());
        }
        const int after = found->fide ? found->fide->id : 0;
        save();
        emit changed();
        if (after != before) {
            found->suggestions.clear();
            found->suggestionsFor = 0;
            setWiki(profileId, {});
            refreshSourceGames(profileId);
            findAccounts(profileId);
        }
    });
}

void ProfileStore::chooseFide(const QString& profileId, int fideId) {
    Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    profile->fide.reset();
    for (const FidePlayer& candidate : profile->fideCandidates) {
        if (candidate.id == fideId) {
            profile->fide = candidate;
        }
    }
    profile->fideChosen = true;
    profile->suggestions.clear();
    profile->suggestionsFor = 0;
    setWiki(profileId, {});
    save();
    emit changed();
    refreshSourceGames(profileId);
    findAccounts(profileId);
}

void ProfileStore::wait() {
    for (QFuture<void>& build : m_builds) {
        build.waitForFinished();
    }
    m_builds.clear();
}

// ---- Downloads ---------------------------------------------------------------------------------

void ProfileStore::sync(const QString& profileId) {
    const Profile* profile = find(profileId);
    if (profile == nullptr || busy(profileId)) {
        return;
    }
    const auto job = std::make_shared<Job>();
    job->profileId = profileId;
    for (const Account& account : profile->accounts) {
        job->accounts.push_back(account.id);
    }
    setStatus(profileId, tr("Starting…"));
    next(job);
}

void ProfileStore::next(const std::shared_ptr<Job>& job) {
    Profile* profile = find(job->profileId);
    if (profile == nullptr) {
        return;
    }
    while (!job->accounts.empty()) {
        const QString id = job->accounts.front();
        job->accounts.pop_front();
        for (Account& account : profile->accounts) {
            if (account.id != id) {
                continue;
            }
            QDir().mkpath(QDir(folderOf(job->profileId)).filePath(account.id));
            if (account.site == AccountSite::Lichess) {
                downloadLichess(job, account);
            } else {
                downloadChessCom(job, account);
            }
            return;
        }
    }
    // The FIDE player's tournament games too, then everything is built.
    setStatus(job->profileId, tr("Looking for tournament games…"));
    m_sourcesPending.remove(job->profileId);
    collectSourceGames(job->profileId, [this, id = job->profileId](bool) { build(id); });
}

void ProfileStore::downloadLichess(const std::shared_ptr<Job>& job, Account& account) {
    // All finished games since the last download, as PGN with their clocks ([%clk]); main lines,
    // no evals. Games downloaded before clocks were asked for: all of them once more, replacing
    // those files when it worked.
    const bool again = !account.lichessClocks && account.lichessSince > 0;
    QUrl url = m_lichess.resolved(QUrl(QStringLiteral("/api/games/user/") + account.username));
    QUrlQuery query;
    if (account.lichessSince > 0 && !again) {
        query.addQueryItem(QStringLiteral("since"), QString::number(account.lichessSince));
    }
    query.addQueryItem(QStringLiteral("clocks"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("evals"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("opening"), QStringLiteral("true"));
    url.setQuery(query);
    if (lichess::pauseLeftMs() > 0) { // Lichess asked for a minute's break
        emit failed(job->profileId,
                    downloadError(QStringLiteral("Lichess"), account.username, 429, QString()));
        next(job);
        return;
    }
    // Signed in, Lichess sends games faster (and your own games faster still).
    QNetworkRequest request =
        lichess::request(url, m_lichessToken ? m_lichessToken(account.username) : QByteArray());
    request.setRawHeader("Accept", "application/x-chess-pgn");
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QString target = QDir(QDir(folderOf(job->profileId)).filePath(account.id))
                               .filePath(QStringLiteral("%1.pgn").arg(started));
    const auto file = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly)) {
        emit failed(job->profileId, tr("Couldn't write the downloaded games (is the disk full?)."));
        next(job);
        return;
    }
    QNetworkReply* reply = m_network->get(request);
    const auto games = std::make_shared<int>(0);
    const QString name = account.username;
    const QString doing = again ? tr("Downloading %1's games again, with their clocks…")
                                : tr("Downloading %1's games from Lichess…");
    setStatus(job->profileId, doing.arg(name));
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, file, games, job, name, doing] {
        const QByteArray data = reply->readAll();
        file->write(data);
        *games += static_cast<int>(data.count("[Event "));
        setStatus(job->profileId, doing.arg(name) + QStringLiteral(" %1").arg(*games));
    });
    const QString accountId = account.id;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, target, job, accountId, name, started, again] {
                reply->deleteLater();
                file->write(reply->readAll());
                file->close();
                const int status = lichess::status(reply); // a 429 starts the minute's pause
                if (reply->error() != QNetworkReply::NoError || status >= 400) {
                    file->remove();
                    emit failed(job->profileId, downloadError(QStringLiteral("Lichess"), name,
                                                              status, reply->errorString()));
                } else {
                    if (again) { // the new download has them all: the old files go
                        for (const QString& old : pgnFiles(QFileInfo(target).absolutePath())) {
                            QFile::remove(old);
                        }
                    }
                    if (file->size() == 0) {
                        file->remove(); // nothing new
                    } else {
                        file->rename(target);
                    }
                    if (Profile* profile = find(job->profileId)) {
                        for (Account& a : profile->accounts) {
                            if (a.id == accountId) {
                                a.lichessSince = started;
                                a.lichessClocks = true;
                                a.synced = QDateTime::currentDateTime();
                            }
                        }
                        save();
                    }
                }
                fetchRatings(job, accountId);
            });
}

void ProfileStore::downloadChessCom(const std::shared_ptr<Job>& job, Account& account) {
    // chess.com keeps games in monthly archives: the list, then every month not downloaded yet
    // (the current month again each time: it is still being played).
    const QString user = account.username.toLower();
    QNetworkRequest request(
        m_chessCom.resolved(QUrl(QStringLiteral("/pub/player/%1/games/archives").arg(user))));
    request.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
    QNetworkReply* reply = m_network->get(request);
    const QString accountId = account.id;
    const QString name = account.username;
    setStatus(job->profileId, tr("Looking up %1's games on chess.com…").arg(name));
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, accountId, name] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            emit failed(job->profileId, downloadError(QStringLiteral("chess.com"), name, status,
                                                      reply->errorString()));
            next(job);
            return;
        }
        const Profile* profile = find(job->profileId);
        const Account* account = nullptr;
        if (profile != nullptr) {
            for (const Account& a : profile->accounts) {
                if (a.id == accountId) {
                    account = &a;
                }
            }
        }
        if (account == nullptr) {
            next(job);
            return;
        }
        const QJsonArray archives = QJsonDocument::fromJson(reply->readAll())
                                        .object()[QStringLiteral("archives")]
                                        .toArray();
        const auto months = std::make_shared<std::deque<QString>>();
        for (const auto& value : archives) {
            const QString url = value.toString();
            const QString month = url.section(u'/', -2); // "2026/10"
            if (!account->chessComMonths.contains(month) || value == archives.last()) {
                months->push_back(url);
            }
        }
        const QString folder = QDir(folderOf(job->profileId)).filePath(accountId);
        const auto fetchNext = std::make_shared<std::function<void()>>();
        const auto done = std::make_shared<int>(0);
        const auto total = static_cast<int>(months->size());
        *fetchNext = [this, months, folder, job, accountId, name, fetchNext, done, total,
                      archives] {
            if (months->empty()) {
                if (Profile* p = find(job->profileId)) {
                    for (Account& a : p->accounts) {
                        if (a.id != accountId) {
                            continue;
                        }
                        a.synced = QDateTime::currentDateTime();
                        // Every month but the last is complete now.
                        for (const auto& value : archives) {
                            const QString month = value.toString().section(u'/', -2);
                            if (value != archives.last() && !a.chessComMonths.contains(month)) {
                                a.chessComMonths << month;
                            }
                        }
                    }
                    save();
                }
                // The cycle ends: this function lets go of itself, which destroys everything it
                // holds. So what is still needed is copied out first, and only copies are used.
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): see above
                const std::shared_ptr<Job> finished = job;
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): see above
                const QString account = accountId;
                ProfileStore* store = this;
                *fetchNext = nullptr;
                store->fetchRatings(finished, account);
                return;
            }
            const QString url = months->front();
            months->pop_front();
            setStatus(job->profileId, tr("Downloading %1's games from chess.com… %2 of %3 months")
                                          .arg(name)
                                          .arg(++*done)
                                          .arg(total));
            QNetworkRequest monthRequest(QUrl(url + QStringLiteral("/pgn")));
            monthRequest.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
            QNetworkReply* monthReply = m_network->get(monthRequest);
            connect(
                monthReply, &QNetworkReply::finished, this, [monthReply, url, folder, fetchNext] {
                    monthReply->deleteLater();
                    if (monthReply->error() == QNetworkReply::NoError) {
                        QFile file(QDir(folder).filePath(url.section(u'/', -2).replace(u'/', u'-') +
                                                         QStringLiteral(".pgn")));
                        if (file.open(QIODevice::WriteOnly)) {
                            file.write(monthReply->readAll());
                        }
                    }
                    if (*fetchNext) {
                        (*fetchNext)();
                    }
                });
        };
        (*fetchNext)();
    });
}

void ProfileStore::fetchRatings(const std::shared_ptr<Job>& job, const QString& accountId) {
    // The account's current ratings by time control (and its title, on Lichess).
    const Profile* profile = find(job->profileId);
    const Account* account = nullptr;
    if (profile != nullptr) {
        for (const Account& a : profile->accounts) {
            if (a.id == accountId) {
                account = &a;
            }
        }
    }
    if (account == nullptr) {
        next(job);
        return;
    }
    const bool lichess = account->site == AccountSite::Lichess;
    if (lichess && lichess::pauseLeftMs() > 0) { // Lichess asked for a minute's break
        next(job);
        return;
    }
    const QUrl url =
        lichess ? m_lichess.resolved(QUrl(QStringLiteral("/api/user/") + account->username))
                : m_chessCom.resolved(QUrl(
                      QStringLiteral("/pub/player/%1/stats").arg(account->username.toLower())));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, lichess::userAgent());
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, accountId, lichess] {
        reply->deleteLater();
        if (lichess) {
            lichess::status(reply); // a 429 starts the minute's pause
        }
        Profile* found = find(job->profileId);
        if (found != nullptr && reply->error() == QNetworkReply::NoError) {
            const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();
            for (Account& a : found->accounts) {
                if (a.id != accountId) {
                    continue;
                }
                const std::map<QString, int> before = a.ratings;
                a.ratings.clear();
                a.siteGames = 0;
                if (lichess) {
                    a.siteGames =
                        object[QStringLiteral("count")].toObject()[QStringLiteral("all")].toInt();
                    const QJsonObject perfs = object[QStringLiteral("perfs")].toObject();
                    for (const char* speed :
                         {"bullet", "blitz", "rapid", "classical", "correspondence"}) {
                        const QJsonObject perf = perfs[QString::fromLatin1(speed)].toObject();
                        // Provisional ratings (few games) and unplayed ones are left out.
                        if (perf[QStringLiteral("games")].toInt() > 0 &&
                            !perf[QStringLiteral("prov")].toBool()) {
                            a.ratings[QString::fromLatin1(speed)] =
                                perf[QStringLiteral("rating")].toInt();
                        }
                    }
                    a.title = object[QStringLiteral("title")].toString();
                } else {
                    for (const auto& [key, speed] :
                         {std::pair{"chess_bullet", "bullet"}, std::pair{"chess_blitz", "blitz"},
                          std::pair{"chess_rapid", "rapid"}, std::pair{"chess_daily", "daily"}}) {
                        const QJsonObject type = object[QString::fromLatin1(key)].toObject();
                        const int rating = type[QStringLiteral("last")]
                                               .toObject()[QStringLiteral("rating")]
                                               .toInt();
                        if (rating > 0) {
                            a.ratings[QString::fromLatin1(speed)] = rating;
                        }
                        const QJsonObject record = type[QStringLiteral("record")].toObject();
                        a.siteGames += record[QStringLiteral("win")].toInt() +
                                       record[QStringLiteral("loss")].toInt() +
                                       record[QStringLiteral("draw")].toInt();
                    }
                }
                // How each rating moved since the last sync (kept until it moves again).
                for (const auto& [speed, rating] : a.ratings) {
                    if (const auto old = before.find(speed);
                        old != before.end() && old->second != rating) {
                        a.ratingChanges[speed] = rating - old->second;
                    }
                }
            }
            save();
        }
        next(job);
    });
}

QStringList ProfileStore::namesOf(const Profile& profile, const QString& part) {
    // Who the profile is in its games: the account's username, or (over the board) its own
    // name and its FIDE name, as "First Last" and as "Last, First".
    QStringList names;
    // ChessBase writes the first name short: "Ott,L", "Ott, L", "Ott, L.".
    const auto initials = [&names](const QString& last, const QString& first) {
        if (!first.isEmpty()) {
            const QString initial = first.left(1);
            names << last + u',' + initial << last + QStringLiteral(", ") + initial
                  << last + QStringLiteral(", ") + initial + u'.' << last + u',' + initial + u'.';
        }
    };
    const auto person = [&names, &initials](const QString& name) {
        const QStringList words = name.split(u' ', Qt::SkipEmptyParts);
        if (name.contains(u',')) { // "Ott, Lukas"
            names << name.simplified();
            const QString last = name.section(u',', 0, 0).trimmed();
            const QString first = name.section(u',', 1).trimmed();
            names << first + u' ' + last << last + u' ' + first << last + u',' + first;
            initials(last, first);
        } else if (words.size() >= 2) { // "Lukas Ott"
            const QString& last = words.last();
            const QString first = words.mid(0, words.size() - 1).join(u' ');
            names << name.simplified() << last + QStringLiteral(", ") + first << last + u' ' + first
                  << last + u',' + first;
            initials(last, first);
        } else if (!words.isEmpty()) {
            names << words.front();
        }
    };
    // Several parts chosen ("otb+src-caissabase"): who the profile is in any of them.
    const QStringList chosen = splitParts(part);
    const auto has = [&chosen](const QString& one) {
        return chosen.isEmpty() || chosen.contains(one);
    };
    for (const Account& account : profile.accounts) {
        if (has(account.id)) {
            names << account.username;
        }
    }
    const bool overTheBoard =
        chosen.isEmpty() || std::any_of(chosen.begin(), chosen.end(), [](const QString& one) {
            return one == QString::fromLatin1(kOtb) ||
                   one.startsWith(QString::fromLatin1(kSourcePrefix));
        });
    if (overTheBoard) {
        person(profile.name);
        if (profile.fide) {
            person(profile.fide->name);
        }
    }
    names.removeDuplicates();
    return names;
}

QStringList ProfileStore::splitParts(const QString& key) {
    QStringList parts = key.split(u'+', Qt::SkipEmptyParts);
    parts.sort();
    parts.removeDuplicates();
    return parts;
}

QString ProfileStore::partsKey(QStringList parts) {
    parts.sort();
    parts.removeDuplicates();
    return parts.join(u'+');
}

void ProfileStore::computeRecords(const QString& profileId) {
    const Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    // Each part's results with the names that are the profile there; in the background.
    std::vector<std::tuple<QString, std::shared_ptr<const gamedb::Database>, QStringList>> work;
    if (profile->all) {
        work.emplace_back(QString(), profile->all, namesOf(*profile, {}));
    }
    for (const auto& [part, db] : profile->parts) {
        work.emplace_back(part, db, namesOf(*profile, part));
    }
    using Records = std::map<QString, gamedb::Record>;
    auto* watcher =
        new QFutureWatcher<Records>(this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<Records>::finished, this, [this, watcher, profileId] {
        watcher->deleteLater();
        if (Profile* p = find(profileId)) {
            p->records = watcher->result();
            emit changed();
        }
    });
    watcher->setFuture(QtConcurrent::run([work = std::move(work)] {
        Records records;
        for (const auto& [part, db, names] : work) {
            std::vector<std::string> list;
            for (const QString& name : names) {
                list.push_back(name.toStdString());
            }
            records[part] = db->record(list);
        }
        return records;
    }));
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

// ---- Databases ---------------------------------------------------------------------------------

void ProfileStore::build(const QString& profileId) {
    const Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    setStatus(profileId, tr("Building the database…"));
    QStringList accounts;
    for (const Account& account : profile->accounts) {
        accounts << account.id;
    }
    const QString folder = folderOf(profileId);
    const QString name = profile->name;
    const auto future = QtConcurrent::run([folder, accounts, name] {
        // Over the board: your files, and each database's games (parts of their own).
        // The databases first: where a game is in several, theirs is kept in all games (full
        // names, evaluations) rather than a pasted copy.
        const QStringList overTheBoard =
            sourceParts(folder) + QStringList{QString::fromLatin1(kOtb)};
        QStringList everything;
        QStringList boardFiles;
        for (const QString& part : accounts + overTheBoard) {
            const QStringList files = pgnFiles(QDir(folder).filePath(part));
            const QString db = QDir(folder).filePath(QStringLiteral("db/") + part);
            if (files.isEmpty()) {
                QDir(db).removeRecursively();
                continue;
            }
            (overTheBoard.contains(part) ? boardFiles : everything) << files;
            buildDatabase(files, db, name);
        }
        // All games: the same game from several over-the-board sources (your PGN, Lichess's
        // broadcasts, Caissabase) once.
        if (!boardFiles.isEmpty()) {
            const QString combined = QDir(folder).filePath(QStringLiteral("db/over-the-board.pgn"));
            if (writeOnce(boardFiles, combined)) {
                everything << combined;
            }
        }
        // Combinations of parts ("Games from" with several chosen) are made again when asked.
        const QStringList combinations =
            QDir(QDir(folder).filePath(QStringLiteral("db")))
                .entryList({QStringLiteral("sel-*")},
                           QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        for (const QString& old : combinations) {
            const QString path = QDir(folder).filePath(QStringLiteral("db/") + old);
            QFileInfo(path).isDir() ? (void)QDir(path).removeRecursively()
                                    : (void)QFile::remove(path);
        }
        const QString all =
            QDir(folder).filePath(QStringLiteral("db/") + QString::fromLatin1(kAll));
        if (everything.isEmpty()) {
            QDir(all).removeRecursively();
        } else {
            buildDatabase(everything, all, name);
        }
    });
    m_builds.push_back(future);
    auto* watcher =
        new QFutureWatcher<void>(this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, profileId] {
        watcher->deleteLater();
        // Combinations of parts are of the games before: made again when next asked for.
        std::erase_if(m_combined, [&profileId](const auto& entry) {
            return entry.first.startsWith(profileId + u'|');
        });
        openDatabases(profileId);
        setStatus(profileId, {});
        emit changed();
    });
    watcher->setFuture(future);
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void ProfileStore::openDatabases(const QString& profileId) {
    Profile* profile = find(profileId);
    if (profile == nullptr) {
        return;
    }
    const QString db = QDir(folderOf(profileId)).filePath(QStringLiteral("db"));
    const auto open = [](const QString& folder) -> std::shared_ptr<const gamedb::Database> {
        if (!QFileInfo::exists(folder)) {
            return nullptr;
        }
        auto opened = gamedb::Database::open(folder.toStdString());
        return opened.ok() ? std::shared_ptr<const gamedb::Database>(std::move(*opened)) : nullptr;
    };
    profile->all = open(QDir(db).filePath(QString::fromLatin1(kAll)));
    profile->parts.clear();
    QStringList parts{QString::fromLatin1(kOtb)};
    for (const Account& account : profile->accounts) {
        parts << account.id;
    }
    // The databases' parts, with the names of the databases they come from.
    profile->sources.clear();
    for (const QString& part : sourceParts(folderOf(profileId))) {
        parts << part;
        QFile label(QDir(folderOf(profileId)).filePath(part + QStringLiteral("/name.txt")));
        profile->sources.emplace_back(
            part, label.open(QIODevice::ReadOnly) ? QString::fromUtf8(label.readAll()) : part);
    }
    for (const QString& part : parts) {
        if (auto opened = open(QDir(db).filePath(part))) {
            profile->parts[part] = std::move(opened);
        }
    }
    computeRecords(profileId);
}

} // namespace kiebitz::app
