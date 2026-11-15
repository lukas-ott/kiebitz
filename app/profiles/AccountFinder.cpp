#include "AccountFinder.hpp"

#include "net/Lichess.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QUrlQuery>

#include <algorithm>
#include <cstdlib>

namespace kiebitz::app {
namespace {

/// Lower case, accents gone, letters only: "Müller-Weiß" → "mullerweiß" → as close as ASCII
/// usernames get.
QString plain(const QString& text) {
    QString result;
    for (const QChar c : text.normalized(QString::NormalizationForm_KD)) {
        if (c.isLetter() && c.unicode() < 0x250) {
            result += c.toLower();
        }
    }
    return result;
}

/// The surname and the first given name, plain: "Wang, Justin Zhide" → ("wang", "justin").
std::pair<QString, QString> nameParts(const QString& fideName) {
    const QString last = fideName.section(u',', 0, 0);
    const QString given = fideName.section(u',', 1).trimmed();
    return {plain(last), plain(given.section(u' ', 0, 0))};
}

/// The titles chess.com lists players by.
const QStringList& chessComTitles() {
    static const QStringList titles{
        QStringLiteral("GM"), QStringLiteral("WGM"), QStringLiteral("IM"), QStringLiteral("WIM"),
        QStringLiteral("FM"), QStringLiteral("WFM"), QStringLiteral("NM"), QStringLiteral("WNM"),
        QStringLiteral("CM"), QStringLiteral("WCM")};
    return titles;
}

constexpr int kShownFrom = 50;
constexpr std::size_t kShownAtMost = 5;

} // namespace

QStringList usernameGuesses(const QString& fideName) {
    const auto [last, first] = nameParts(fideName);
    QStringList guesses;
    if (last.isEmpty()) {
        return guesses;
    }
    if (!first.isEmpty()) {
        guesses << first + last << last + first << first + u'_' + last << first + u'-' + last
                << first.left(1) + last << last + first.left(1) << last + u'_' + first;
    }
    guesses << last;
    guesses.removeDuplicates();
    guesses.removeIf([](const QString& guess) { return guess.size() < 3; });
    return guesses;
}

AccountSuggestion judgeAccount(const AccountFacts& facts, const FidePlayer& player) {
    AccountSuggestion suggestion;
    suggestion.site = facts.site;
    suggestion.username = facts.username;
    const auto [last, first] = nameParts(player.name);
    if (last.isEmpty()) {
        return suggestion;
    }
    if (facts.wikidata) {
        suggestion.score += 100; // people who know the player wrote it down
        suggestion.reasons << AccountFinder::tr("Listed on Wikidata");
    }
    const QString real = plain(facts.realName);
    if (!real.isEmpty() && real.contains(last)) {
        if (!first.isEmpty() && real.contains(first)) {
            suggestion.score += 50;
            suggestion.reasons << AccountFinder::tr("Real name matches");
        } else {
            suggestion.score += 20;
            suggestion.reasons << AccountFinder::tr("Surname in the real name");
        }
    }
    if (facts.fideRating > 0 && player.standard > 0) {
        const int apart = std::abs(facts.fideRating - player.standard);
        if (apart <= 50) {
            suggestion.score += 40;
            suggestion.reasons << AccountFinder::tr("FIDE %1 in the profile").arg(facts.fideRating);
        } else if (apart > 150) {
            suggestion.score -= 40; // someone else's rating
        }
    }
    if (!facts.title.isEmpty() && facts.title == player.title) {
        suggestion.score += 25;
        suggestion.reasons << AccountFinder::tr("Verified %1").arg(facts.title);
    }
    if (plain(facts.username).contains(last)) {
        suggestion.score += 10;
        suggestion.reasons << AccountFinder::tr("Name in the username");
    }
    return suggestion;
}

struct AccountFinder::Search {
    FidePlayer player;
    std::function<void(std::vector<AccountSuggestion>, WikiPerson)> done;
    WikiPerson wiki;
    int pending = 0;            ///< requests not answered yet
    int lichessSearches = 0;    ///< Lichess username searches not answered yet
    QSet<QString> lichessNames; ///< candidates, lower case
    QSet<QString> chessComAsked;
    std::vector<AccountFacts> facts;
};

AccountFinder::AccountFinder(QNetworkAccessManager& network, QObject* parent)
    : QObject(parent), m_network(network) {}

void AccountFinder::find(const FidePlayer& player,
                         std::function<void(std::vector<AccountSuggestion>, WikiPerson)> done) {
    const auto search = std::make_shared<Search>();
    search->player = player;
    search->done = std::move(done);
    search->pending = 1; // this call, so nothing ends before every request is out
    const bool lichess = lichess::pauseLeftMs() == 0;
    wikidata(search, lichess);
    if (lichess) {
        lichessCandidates(search);
    }
    chessComCandidates(search);
    finishOne(search);
}

void AccountFinder::finishOne(const std::shared_ptr<Search>& search) {
    if (--search->pending > 0) {
        return;
    }
    std::vector<AccountSuggestion> suggestions;
    QSet<QString> seen;
    for (const AccountFacts& facts : search->facts) {
        const QString key =
            QString::number(static_cast<int>(facts.site)) + u':' + facts.username.toLower();
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        AccountSuggestion suggestion = judgeAccount(facts, search->player);
        if (suggestion.score >= kShownFrom) {
            suggestions.push_back(std::move(suggestion));
        }
    }
    std::stable_sort(suggestions.begin(), suggestions.end(),
                     [](const auto& a, const auto& b) { return a.score > b.score; });
    if (suggestions.size() > kShownAtMost) {
        suggestions.resize(kShownAtMost);
    }
    search->done(std::move(suggestions), search->wiki);
}

void AccountFinder::wikidata(const std::shared_ptr<Search>& search, bool lichess) {
    if (search->player.id <= 0) {
        return;
    }
    QUrl url = m_wikidata.resolved(QUrl(QStringLiteral("/sparql")));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), wikidataQuery(search->player.id));
    url.setQuery(query);
    QNetworkRequest request = lichess::request(url, {}, lichess::kTimeoutMs);
    request.setRawHeader("Accept", "application/sparql-results+json");
    ++search->pending;
    if (lichess) {
        ++search->lichessSearches; // its Lichess accounts are looked at with the others
    }
    QNetworkReply* reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, search, lichess] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            search->wiki = parseWikidata(reply->readAll());
            for (const QString& name : search->wiki.lichess) {
                search->lichessNames.insert(name.toLower());
            }
        }
        if (lichess && --search->lichessSearches == 0) {
            lichessProfiles(search);
        }
        finishOne(search);
    });
}

void AccountFinder::lichessCandidates(const std::shared_ptr<Search>& search) {
    // Lichess searches usernames by their beginning: the guesses, and the surname alone.
    const QStringList terms = usernameGuesses(search->player.name).mid(0, 5);
    for (const QString& term : terms) {
        QUrl url = m_lichess.resolved(QUrl(QStringLiteral("/api/player/autocomplete")));
        url.setQuery(QStringLiteral("term=") + term);
        ++search->pending;
        ++search->lichessSearches;
        QNetworkReply* reply = m_network.get(lichess::request(url, {}, lichess::kTimeoutMs));
        connect(reply, &QNetworkReply::finished, this, [this, reply, search] {
            reply->deleteLater();
            if (lichess::status(reply) == 200) {
                for (const auto& name : QJsonDocument::fromJson(reply->readAll()).array()) {
                    if (search->lichessNames.size() < 100) {
                        search->lichessNames.insert(name.toString().toLower());
                    }
                }
            }
            if (--search->lichessSearches == 0) {
                lichessProfiles(search);
            }
            finishOne(search);
        });
    }
}

void AccountFinder::lichessProfiles(const std::shared_ptr<Search>& search) {
    if (search->lichessNames.isEmpty()) {
        return;
    }
    // Their profiles in one request: real name, FIDE rating, title.
    const QStringList names(search->lichessNames.begin(), search->lichessNames.end());
    QNetworkRequest request = lichess::request(
        m_lichess.resolved(QUrl(QStringLiteral("/api/users"))), {}, lichess::kTimeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain"));
    ++search->pending;
    QNetworkReply* reply = m_network.post(request, names.join(u',').toUtf8());
    connect(reply, &QNetworkReply::finished, this, [reply, search] {
        reply->deleteLater();
        if (lichess::status(reply) == 200) {
            for (const auto& value : QJsonDocument::fromJson(reply->readAll()).array()) {
                const QJsonObject user = value.toObject();
                const QJsonObject profile = user[QStringLiteral("profile")].toObject();
                AccountFacts facts;
                facts.site = AccountSite::Lichess;
                facts.username = user[QStringLiteral("username")].toString();
                facts.realName = profile[QStringLiteral("realName")].toString();
                facts.title = user[QStringLiteral("title")].toString();
                facts.fideRating = profile[QStringLiteral("fideRating")].toInt();
                facts.wikidata =
                    std::any_of(search->wiki.lichess.begin(), search->wiki.lichess.end(),
                                [&](const QString& name) {
                                    return name.compare(facts.username, Qt::CaseInsensitive) == 0;
                                });
                if (!facts.username.isEmpty() && !user[QStringLiteral("disabled")].toBool()) {
                    search->facts.push_back(facts);
                }
            }
        }
        finishOne(search);
    });
}

void AccountFinder::chessComCandidates(const std::shared_ptr<Search>& search) {
    // chess.com can't be searched: the likely usernames, one by one...
    for (const QString& guess : usernameGuesses(search->player.name)) {
        chessComProfile(search, guess);
    }
    // ... and for a titled player, the site's list of that title, by name.
    if (!chessComTitles().contains(search->player.title)) {
        return;
    }
    ++search->pending;
    QNetworkReply* reply = m_network.get(lichess::request(
        m_chessCom.resolved(QUrl(QStringLiteral("/pub/titled/") + search->player.title)), {},
        lichess::kTimeoutMs));
    connect(reply, &QNetworkReply::finished, this, [this, reply, search] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            const QString last = nameParts(search->player.name).first;
            const QJsonArray players = QJsonDocument::fromJson(reply->readAll())
                                           .object()[QStringLiteral("players")]
                                           .toArray();
            int asked = 0;
            for (const auto& name : players) {
                if (asked < 10 && !last.isEmpty() && plain(name.toString()).contains(last)) {
                    chessComProfile(search, name.toString().toLower());
                    ++asked;
                }
            }
        }
        finishOne(search);
    });
}

void AccountFinder::chessComProfile(const std::shared_ptr<Search>& search,
                                    const QString& username) {
    if (search->chessComAsked.contains(username)) {
        return;
    }
    search->chessComAsked.insert(username);
    ++search->pending;
    QNetworkReply* reply = m_network.get(
        lichess::request(m_chessCom.resolved(QUrl(QStringLiteral("/pub/player/") + username)), {},
                         lichess::kTimeoutMs));
    connect(reply, &QNetworkReply::finished, this, [this, reply, search, username] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            finishOne(search); // no such account
            return;
        }
        const QJsonObject player = QJsonDocument::fromJson(reply->readAll()).object();
        AccountFacts facts;
        facts.site = AccountSite::ChessCom;
        facts.username = player[QStringLiteral("username")].toString(username);
        facts.realName = player[QStringLiteral("name")].toString();
        facts.title = player[QStringLiteral("title")].toString();
        // The FIDE rating the user put in, among the stats.
        ++search->pending;
        QNetworkReply* stats = m_network.get(lichess::request(
            m_chessCom.resolved(QUrl(QStringLiteral("/pub/player/%1/stats").arg(username))), {},
            lichess::kTimeoutMs));
        connect(stats, &QNetworkReply::finished, this, [stats, search, facts]() mutable {
            stats->deleteLater();
            if (stats->error() == QNetworkReply::NoError) {
                facts.fideRating = QJsonDocument::fromJson(stats->readAll())
                                       .object()[QStringLiteral("fide")]
                                       .toInt();
            }
            search->facts.push_back(facts);
            finishOne(search);
        });
        finishOne(search);
    });
}

} // namespace kiebitz::app
