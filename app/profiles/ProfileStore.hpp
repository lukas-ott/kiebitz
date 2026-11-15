#pragma once

#include "Fide.hpp"
#include "Wikidata.hpp"
#include "kiebitz/core/game.hpp"
#include "kiebitz/gamedb/database.hpp"

#include <QDateTime>
#include <QFuture>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QNetworkAccessManager;

namespace kiebitz::app {

/// Where an account's games come from.
enum class AccountSite : std::uint8_t { Lichess, ChessCom };

/// An online account of a profile: its games are downloaded by username (public, no login).
struct Account {
    QString id; ///< "lichess-name", "chesscom-name": also its folder
    AccountSite site = AccountSite::Lichess;
    QString username;
    QDateTime synced;        ///< the last download (invalid: never)
    qint64 lichessSince = 0; ///< Lichess: games after this time (ms) are new
    /// Lichess: its games were downloaded with their clocks. Accounts from before that are
    /// downloaded once more, whole.
    bool lichessClocks = false;
    QStringList chessComMonths; ///< chess.com: months downloaded for good ("2026/10")
    QString title;              ///< on the site ("FM"), or empty
    /// The current ratings by time control ("bullet", "blitz", "rapid", "classical",
    /// "correspondence"; chess.com: "daily"), as the site has them.
    std::map<QString, int> ratings;
    std::map<QString, int> ratingChanges; ///< how each moved at the last sync that changed it
    int siteGames = 0;                    ///< games on the site (the downloads may be fewer)
};

/// An account that may be the profile's FIDE player's (see AccountFinder).
struct SuggestedAccount {
    AccountSite site = AccountSite::Lichess;
    QString username;
    QStringList reasons;
};

/// A person whose games Kiebitz keeps: you (the main profile), or a friend or opponent. All of a
/// profile's games, from its accounts and the over-the-board games added to it, are one
/// database; each account (and the over-the-board games) is also a database of its own, so it
/// can be looked at, and explored, alone.
struct Profile {
    QString id;
    QString name;
    bool main = false;
    std::vector<Account> accounts;
    /// The FIDE player with this name: the closest match, or the one chosen. Looked up when the
    /// profile is made or renamed (names of two words or more).
    std::optional<FidePlayer> fide;
    std::vector<FidePlayer> fideCandidates; ///< everyone who fits the name, best first
    bool fideChosen = false;  ///< picked by hand (or "none of these"): not replaced by a lookup
    bool fideLooked = false;  ///< a lookup has been done for this name
    bool fideLooking = false; ///< a lookup is under way
    QString fideError;        ///< why the last lookup failed (FIDE not answering), or empty
    /// Online accounts that may be the FIDE player's, best first, looked for once per player.
    std::vector<SuggestedAccount> suggestions;
    int suggestionsFor = 0;  ///< the FIDE id they were looked for (0: not yet)
    bool suggesting = false; ///< looking right now
    QStringList dismissed;   ///< "Not them": "lichess:name", "chesscom:name"
    /// What Wikidata knows about the FIDE player (looked up with the accounts): a photo,
    /// Wikipedia articles; most players aren't there.
    WikiPerson wiki;
    std::shared_ptr<const gamedb::Database> all; ///< null until built (or no games)
    /// By part: an account's id, "otb" (your PGN files, games pasted), or a database's part
    /// ("src-lichess-broadcasts": the player's games found in that database).
    std::map<QString, std::shared_ptr<const gamedb::Database>> parts;
    /// The database parts there are: their part id and the database's name.
    std::vector<std::pair<QString, QString>> sources;
    /// The profile's results by part ("" = all its games), counted from its games.
    std::map<QString, gamedb::Record> records;
};

/// The profiles, kept in "Profiles" under the app's data folder:
///   profiles.json                 who they are and their accounts;
///   <profile>/<account>/*.pgn     the games as downloaded (or added: <profile>/otb/);
///   <profile>/src-<db>/games.pgn  the player's games found in a game database (name.txt: its
///                                 name);
///   <profile>/db/<part>/          the databases built from them (part: an account, "otb",
///                                 a database's, or "all").
/// Downloads and building run in the background; changed() and progress() tell the page.
class ProfileStore : public QObject {
    Q_OBJECT

public:
    /// `root`: where profiles live (empty: the default).
    explicit ProfileStore(const QString& root = {}, QObject* parent = nullptr);
    /// Where the profiles are kept.
    [[nodiscard]] const QString& root() const noexcept { return m_root; }
    ~ProfileStore() override;
    ProfileStore(const ProfileStore&) = delete;
    ProfileStore& operator=(const ProfileStore&) = delete;
    ProfileStore(ProfileStore&&) = delete;
    ProfileStore& operator=(ProfileStore&&) = delete;

    [[nodiscard]] static QString defaultRoot();
    [[nodiscard]] const std::vector<Profile>& profiles() const { return m_profiles; }
    [[nodiscard]] const Profile* profile(const QString& id) const;

    /// A new profile; returns its id.
    QString addProfile(const QString& name);
    void renameProfile(const QString& id, const QString& name);
    /// Removes a profile and its games (not the main one).
    void removeProfile(const QString& id);

    /// Asks the site whether an account exists. `done` gets its name as the site writes it, or
    /// empty if there is no such account; `error` is set when the site couldn't be asked.
    void checkAccount(AccountSite site, const QString& username,
                      std::function<void(const QString& name, const QString& error)> done);
    /// Adds an account and downloads its games. Returns its id, or empty if it is there already.
    QString addAccount(const QString& profileId, AccountSite site, const QString& username);
    void removeAccount(const QString& profileId, const QString& accountId);
    /// Downloads the new games of all of a profile's accounts, then rebuilds its databases.
    void sync(const QString& profileId);
    /// Adds over-the-board games (PGN files) to a profile.
    /// Adds PGN files' games to the profile's over-the-board games. Games it already has (the same
    /// players, date, result and moves) are left out, so adding a file twice changes nothing;
    /// otbAdded() says how many were new.
    void addOtbGames(const QString& profileId, const QStringList& files);
    /// Deletes a game from the profile's files (one copy of it: a duplicate keeps the other) and
    /// rebuilds. `part`: where to look ("" = everywhere: every account and the OTB games).
    void deleteGame(const QString& profileId, const QString& part, const core::Game& game);
    /// What a part's database is right now ("" = all its games): changes whenever it is built
    /// again (a sync, games added or deleted). For caches of what was worked out from it.
    [[nodiscard]] QString databaseStamp(const QString& profileId, const QString& part) const;
    /// A file in the profile's folder for such a cache (its folder is made).
    [[nodiscard]] QString cacheFile(const QString& profileId, const QString& name) const;
    /// A game as its PGN file has it, with its clocks and comments (the database keeps only the
    /// moves), found in the background; `done` gets it, or the game as given if it isn't found.
    void originalGame(const QString& profileId, const QString& part, const core::Game& game,
                      std::function<void(const core::Game&)> done);

    /// Who the profile is in a part's games ("" = all): usernames, and its names over the board.
    [[nodiscard]] static QStringList namesOf(const Profile& profile, const QString& part);

    // A "part" may be several, chosen together: "otb+src-caissabase" (partsKey()). Everything
    // above that takes a part takes such a key too.
    /// The parts of a key, sorted ("" = none: all games).
    [[nodiscard]] static QStringList splitParts(const QString& key);
    /// The key of several parts (any order, once each).
    [[nodiscard]] static QString partsKey(QStringList parts);
    /// The games of a part, or of several together (each game once); null if there are none,
    /// or several aren't combined yet (combine() does that).
    [[nodiscard]] std::shared_ptr<const gamedb::Database> database(const QString& profileId,
                                                                   const QString& part) const;
    /// Combines several parts' games into one database, each game once (in the background;
    /// changed() follows). Kept until the profile's games change.
    void combine(const QString& profileId, const QString& part);

    /// Looks up the profile's FIDE player again (in the background; changed() follows).
    void lookUpFide(const QString& profileId);
    /// Picks one of the candidates as the profile's FIDE player (0: none of them).
    void chooseFide(const QString& profileId, int fideId);

    /// What is going on with a profile ("Downloading CatalanSimp's games…"), or empty.
    [[nodiscard]] QString status(const QString& profileId) const;
    [[nodiscard]] bool busy(const QString& profileId) const;

    /// The servers (tests point them at a fake one). `wiki` is Wikidata's query service and
    /// Wikimedia Commons; without it, a `fide` server given is used for those too.
    void setServers(const QUrl& lichess, const QUrl& chessCom, const QUrl& fide = {},
                    const QUrl& wiki = {});
    /// The player's photo (from Wikimedia Commons), saved with the profile; empty if none.
    [[nodiscard]] QString photoFile(const QString& profileId) const;
    /// The Lichess token to download an account's games with (its own, else any; empty: not
    /// signed in). Downloads with one go faster, an account's own games faster still.
    void setLichessToken(std::function<QByteArray(const QString& username)> token) {
        m_lichessToken = std::move(token);
    }
    /// The game databases a player's games are looked for in, with their names (Lichess's
    /// broadcasts, Caissabase, ...). Called on this thread whenever they are looked for.
    using GameSources =
        std::function<std::vector<std::pair<QString, std::shared_ptr<const gamedb::Database>>>()>;
    void setGameSources(GameSources sources) { m_gameSources = std::move(sources); }
    /// Whether there is a database to look in.
    [[nodiscard]] bool hasGameSources() const;
    /// Looks for the games of the profile's FIDE player in each of those databases (in the
    /// background): by FIDE id where it has them, else by name. Each database's games are a
    /// part of their own ("Games from"); games deleted from them stay deleted. Builds again
    /// only if they changed. Busy: done once it isn't. Without a FIDE player: nothing.
    void refreshSourceGames(const QString& profileId);

    /// Looks for online accounts that may be the profile's FIDE player's (in the background;
    /// changed() follows). Once per player, unless `again`.
    void findAccounts(const QString& profileId, bool again = false);
    /// "Not them": the suggestion goes, and isn't suggested again.
    void dismissSuggestion(const QString& profileId, AccountSite site, const QString& username);

    /// Waits for running builds (tests, quitting).
    void wait();

signals:
    void changed();
    void progress(const QString& profileId);
    void failed(const QString& profileId, const QString& message);
    /// Over-the-board games added: `added` new ones, `skipped` already there.
    void otbAdded(const QString& profileId, int added, int skipped);

private:
    struct Job; ///< a sync in progress

    void load();
    void save() const;
    [[nodiscard]] Profile* find(const QString& id);
    [[nodiscard]] QString folderOf(const QString& profileId) const;
    /// Where a part's PGN files are ("" = every account's and the OTB games').
    [[nodiscard]] QStringList partFolders(const Profile& profile, const QString& part) const;
    void setStatus(const QString& profileId, const QString& status);
    void next(const std::shared_ptr<Job>& job);
    void downloadLichess(const std::shared_ptr<Job>& job, Account& account);
    void downloadChessCom(const std::shared_ptr<Job>& job, Account& account);
    void build(const QString& profileId);
    void fetchRatings(const std::shared_ptr<Job>& job, const QString& accountId);
    void computeRecords(const QString& profileId);
    /// Takes what Wikidata says about the profile's player; fetches the photo if it is new.
    void setWiki(const QString& profileId, const WikiPerson& wiki);
    void openDatabases(const QString& profileId);
    /// The player's games from each database, as refreshSourceGames() writes them; `done` (on
    /// this thread) gets whether anything changed.
    void collectSourceGames(const QString& profileId, std::function<void(bool changed)> done);

    QString m_root;
    std::vector<Profile> m_profiles;
    std::map<QString, QString> m_status; ///< profile id → what it is doing
    QUrl m_lichess{QStringLiteral("https://lichess.org")};
    QUrl m_chessCom{QStringLiteral("https://api.chess.com")};
    std::function<QByteArray(const QString& username)> m_lichessToken;
    QUrl m_fide{QStringLiteral("https://lichess.org")}; ///< Lichess keeps FIDE's rating list
    QUrl m_commons{QStringLiteral("https://commons.wikimedia.org")};
    QNetworkAccessManager* m_network = nullptr;
    class AccountFinder* m_finder = nullptr;
    std::vector<QFuture<void>> m_builds;
    GameSources m_gameSources;
    QSet<QString> m_sourcesPending; ///< refreshSourceGames() asked for while busy
    /// Several parts' games together, by "profile|key".
    std::map<QString, std::shared_ptr<const gamedb::Database>> m_combined;
    QSet<QString> m_combining; ///< being combined
};

} // namespace kiebitz::app
