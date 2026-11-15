#pragma once

#include "Fide.hpp"
#include "ProfileStore.hpp"
#include "Wikidata.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <vector>

class QNetworkAccessManager;

namespace kiebitz::app {

/// An online account that may be the FIDE player's, and why.
struct AccountSuggestion {
    AccountSite site = AccountSite::Lichess;
    QString username;
    QStringList reasons; ///< "Real name matches", "FIDE 2103 in the profile", ...
    int score = 0;       ///< how sure: 50 and more is shown
};

/// What a site says about an account, as far as matching a FIDE player goes.
struct AccountFacts {
    AccountSite site = AccountSite::Lichess;
    QString username;
    QString realName;      ///< as the user wrote it (Lichess "realName", chess.com "name")
    QString title;         ///< verified by the site ("GM"), or empty
    int fideRating = 0;    ///< the FIDE rating the user put in the profile, or 0
    bool wikidata = false; ///< Wikidata says it is the FIDE player's
};

/// Usernames people often make from their name ("Wang, Justin Zhide": justinwang,
/// wangjustin, justin_wang, jwang, ...), in lower case and plain letters.
[[nodiscard]] QStringList usernameGuesses(const QString& fideName);

/// How well an account fits the FIDE player, with the reasons (in the app's language).
[[nodiscard]] AccountSuggestion judgeAccount(const AccountFacts& facts, const FidePlayer& player);

/// Looks for a FIDE player's accounts on Lichess and chess.com: usernames made from the name
/// (Lichess's username search; chess.com's profiles one by one), for titled players the
/// site's list of that title, and the Lichess accounts Wikidata lists for the FIDE id; then
/// what each candidate's profile says. Neither site can be
/// searched by real name, so this suggests; the user decides.
class AccountFinder : public QObject {
    Q_OBJECT

public:
    explicit AccountFinder(QNetworkAccessManager& network, QObject* parent = nullptr);

    /// The servers (tests point them at a fake one).
    void setServers(const QUrl& lichess, const QUrl& chessCom, const QUrl& wikidata) {
        m_lichess = lichess;
        m_chessCom = chessCom;
        m_wikidata = wikidata;
    }
    /// Starts looking; `done` gets the suggestions worth showing, best first (at most five),
    /// and what Wikidata knows about the player (an empty item if nothing, or it didn't answer).
    void
    find(const FidePlayer& player,
         std::function<void(std::vector<AccountSuggestion> suggestions, WikiPerson wiki)> done);

private:
    struct Search;
    void lichessCandidates(const std::shared_ptr<Search>& search);
    void wikidata(const std::shared_ptr<Search>& search, bool lichess);
    void lichessProfiles(const std::shared_ptr<Search>& search);
    void chessComCandidates(const std::shared_ptr<Search>& search);
    void chessComProfile(const std::shared_ptr<Search>& search, const QString& username);
    static void finishOne(const std::shared_ptr<Search>& search);

    QNetworkAccessManager& m_network;
    QUrl m_lichess{QStringLiteral("https://lichess.org")};
    QUrl m_chessCom{QStringLiteral("https://api.chess.com")};
    QUrl m_wikidata{QStringLiteral("https://query.wikidata.org")};
};

} // namespace kiebitz::app
