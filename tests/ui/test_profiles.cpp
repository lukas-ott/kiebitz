// Profiles: accounts whose games are downloaded (from a fake Lichess and chess.com, never the
// real ones), over-the-board games added by hand, and the databases built from them.

#include "board/BoardView.hpp"
#include "databases/DatabaseManager.hpp"
#include "explorer/ExplorerPanel.hpp"
#include "explorer/LichessExplorer.hpp"
#include "fake_server.hpp"
#include "kiebitz/pgn/pgn.hpp"
#include "kiebitz/repertoire/gaps.hpp"
#include "library/Library.hpp"
#include "net/Lichess.hpp"
#include "profiles/AccountFinder.hpp"
#include "profiles/Fide.hpp"
#include "profiles/LichessLogin.hpp"
#include "profiles/PreparePage.hpp"
#include "profiles/ProfileStore.hpp"
#include "profiles/ProfilesView.hpp"
#include "profiles/Wikidata.hpp"
#include "repertoire/RepertoireStore.hpp"
#include "sound/Sounds.hpp"
#include "style/AppStyle.hpp"
#include "style/Icons.hpp"
#include "widgets/Message.hpp"
#include "widgets/MultiChoice.hpp"
#include "widgets/RatingChart.hpp"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrlQuery>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>

namespace {

using kiebitz::test::FakeServer;

using kiebitz::app::AccountSite;
using kiebitz::app::ProfileStore;

QByteArray game(const char* white, const char* black, const char* moves) {
    return QByteArray("[Site \"https://lichess.org/x\"]\n[Date \"2025.06.01\"]\n[White \"") +
           white + "\"]\n[Black \"" + black + "\"]\n[Result \"1-0\"]\n\n" + moves + " 1-0\n\n";
}

/// A stand-in for the Keychain and the settings: tokens and usernames in memory, kept across
/// LichessLogin objects (as across starts of the app).
struct FakeKeychain {
    std::map<QString, QByteArray> tokens; ///< by username in lower case
    QStringList names;

    kiebitz::app::LichessLogin::Storage storage() {
        return {
            [this](const QString& user) -> std::optional<QByteArray> {
                const auto found = tokens.find(user.toLower());
                return found != tokens.end() ? std::optional(found->second) : std::nullopt;
            },
            [this](const QString& user, const QByteArray& token) {
                tokens[user.toLower()] = token;
            },
            [this](const QString& user) { tokens.erase(user.toLower()); },
            [this] { return names; },
            [this](const QStringList& value) { names = value; },
        };
    }
};

class ProfilesTest : public ::testing::Test {
protected:
    QTemporaryDir folder;
    FakeServer server;

    static void waitFor(const ProfileStore& store, const QString& id) {
        ASSERT_TRUE(QTest::qWaitFor([&] { return !store.busy(id); }, 15000));
    }
};

} // namespace

TEST_F(ProfilesTest, ThereIsAlwaysAMainProfile) {
    const ProfileStore store(folder.path());
    ASSERT_EQ(store.profiles().size(), 1U);
    EXPECT_TRUE(store.profiles()[0].main);
}

TEST_F(ProfilesTest, LichessGamesAreDownloadedAndThenOnlyNewOnes) {
    server.pages["/api/games/user/alice"] =
        game("alice", "bob", "1. e4 e5 2. Nf3") + game("carol", "alice", "1. d4 d5");
    server.pages["/api/user/alice"] =
        R"({"title":"FM","perfs":{"blitz":{"games":120,"rating":2210,"prov":false},)"
        R"("bullet":{"games":3,"rating":1500,"prov":true}}})";
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString me = store.profiles()[0].id;
    EXPECT_FALSE(store.addAccount(me, AccountSite::Lichess, QStringLiteral("alice")).isEmpty());
    waitFor(store, me);
    const auto* profile = store.profile(me);
    ASSERT_NE(profile->all, nullptr);
    EXPECT_EQ(profile->all->gameCount(), 2U);
    ASSERT_EQ(profile->parts.count(QStringLiteral("lichess-alice")), 1U);
    EXPECT_FALSE(profile->accounts[0].synced.isNull());
    EXPECT_FALSE(server.asked.contains(QStringLiteral("/api/games/user/alice?since"))); // all
    EXPECT_TRUE(std::any_of(server.asked.begin(), server.asked.end(), [](const QString& asked) {
        return asked.startsWith(u"/api/games/user/alice") && asked.contains(u"clocks=true");
    })); // with their clocks
    // Ratings from the site (a provisional one is left out), and the title.
    EXPECT_EQ(profile->accounts[0].title, QStringLiteral("FM"));
    EXPECT_EQ(profile->accounts[0].ratings.at(QStringLiteral("blitz")), 2210);
    EXPECT_FALSE(profile->accounts[0].ratings.contains(QStringLiteral("bullet")));
    // The record by colour: won with White (vs bob), lost with Black (vs carol).
    ASSERT_TRUE(QTest::qWaitFor([&] { return store.profile(me)->records.contains({}); }, 5000));
    const auto& record = store.profile(me)->records.at({});
    EXPECT_EQ(record.asWhite, (std::array<std::uint64_t, 3>{1, 0, 0}));
    EXPECT_EQ(record.asBlack, (std::array<std::uint64_t, 3>{0, 0, 1}));

    // The next sync asks only for games since the last one.
    server.pages["/api/games/user/alice"] = game("alice", "dave", "1. c4 c5");
    store.sync(me);
    waitFor(store, me);
    EXPECT_TRUE(std::any_of(server.asked.begin(), server.asked.end(), [](const QString& asked) {
        return asked.startsWith(u"/api/games/user/alice?since=");
    }));
    EXPECT_EQ(store.profile(me)->all->gameCount(), 3U);
    // The same account twice: not added again.
    EXPECT_TRUE(store.addAccount(me, AccountSite::Lichess, QStringLiteral("Alice")).isEmpty());
}

TEST_F(ProfilesTest, LichessGamesDownloadedWithoutClocksAreDownloadedOnceMore) {
    server.pages["/api/games/user/alice"] =
        game("alice", "bob", "1. e4 e5 2. Nf3") + game("carol", "alice", "1. d4 d5");
    server.pages["/api/user/alice"] = R"({"perfs":{}})";
    {
        ProfileStore store(folder.path());
        store.setServers(server.url(), server.url(), server.url());
        store.addAccount(store.profiles()[0].id, AccountSite::Lichess, QStringLiteral("alice"));
        waitFor(store, store.profiles()[0].id);
        store.wait();
    }
    // As an account from before clocks were asked for (it has no lichessClocks).
    QFile json(QDir(folder.path()).filePath(QStringLiteral("profiles.json")));
    ASSERT_TRUE(json.open(QIODevice::ReadOnly));
    QByteArray text = json.readAll();
    json.close();
    ASSERT_TRUE(text.contains("\"lichessClocks\": true"));
    text.replace("\"lichessClocks\": true", "\"lichessClocks\": false");
    ASSERT_TRUE(json.open(QIODevice::WriteOnly | QIODevice::Truncate));
    json.write(text);
    json.close();

    // The next sync downloads them all again (no "since"), and the copy replaces the old one:
    // the same two games, with a new one, not five.
    server.pages["/api/games/user/alice"] = game("alice", "bob", "1. e4 e5 2. Nf3") +
                                            game("carol", "alice", "1. d4 d5") +
                                            game("alice", "dave", "1. c4 c5");
    server.asked.clear();
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString me = store.profiles()[0].id;
    store.sync(me);
    waitFor(store, me);
    EXPECT_TRUE(std::any_of(server.asked.begin(), server.asked.end(), [](const QString& asked) {
        return asked.startsWith(u"/api/games/user/alice") && !asked.contains(u"since=");
    }));
    EXPECT_EQ(store.profile(me)->all->gameCount(), 3U);
    EXPECT_TRUE(store.profile(me)->accounts[0].lichessClocks);
    // And after that, only what's new again.
    server.asked.clear();
    store.sync(me);
    waitFor(store, me);
    EXPECT_TRUE(std::any_of(server.asked.begin(), server.asked.end(), [](const QString& asked) {
        return asked.startsWith(u"/api/games/user/alice?since=");
    }));
}

TEST_F(ProfilesTest, AnAccountIsLookedUpBeforeItIsAdded) {
    server.pages["/api/user/catalansimp"] = R"({"id":"catalansimp","username":"CatalanSimp"})";
    server.pages["/api/user/gone"] = R"({"id":"gone","username":"Gone","disabled":true})";
    server.pages["/pub/player/bob"] = R"({"username":"bob","player_id":1})";
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    struct Answer {
        QString name;
        QString error;
        bool done = false;
    };
    const auto check = [&](AccountSite site, const char* name) {
        Answer answer;
        store.checkAccount(site, QString::fromUtf8(name),
                           [&answer](const QString& found, const QString& error) {
                               answer = {found, error, true};
                           });
        EXPECT_TRUE(QTest::qWaitFor([&] { return answer.done; }, 5000));
        return answer;
    };
    // There: under the site's own spelling.
    EXPECT_EQ(check(AccountSite::Lichess, "catalansimp").name, QStringLiteral("CatalanSimp"));
    EXPECT_EQ(check(AccountSite::ChessCom, "Bob").name, QStringLiteral("Bob"));
    // Not there (404), or closed: no name, no error.
    const Answer missing = check(AccountSite::Lichess, "lsadkjföd");
    EXPECT_TRUE(missing.name.isEmpty());
    EXPECT_TRUE(missing.error.isEmpty());
    EXPECT_TRUE(check(AccountSite::Lichess, "gone").name.isEmpty());
    // The site away: an error, so nothing is added.
    store.setServers(QUrl(QStringLiteral("http://127.0.0.1:1")), server.url(), server.url());
    EXPECT_FALSE(check(AccountSite::Lichess, "catalansimp").error.isEmpty());
}

TEST_F(ProfilesTest, ChessComMonthsAreDownloadedOnce) {
    const QByteArray base = server.url().toString().toUtf8();
    server.pages["/pub/player/bob/games/archives"] = R"({"archives":[")" + base +
                                                     R"(/pub/player/bob/games/2026/09",")" + base +
                                                     R"(/pub/player/bob/games/2026/10"]})";
    server.pages["/pub/player/bob/games/2026/09/pgn"] = game("Bob", "x", "1. e4 c5");
    server.pages["/pub/player/bob/games/2026/10/pgn"] = game("Bob", "y", "1. d4 Nf6");
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString me = store.profiles()[0].id;
    store.addAccount(me, AccountSite::ChessCom, QStringLiteral("Bob"));
    waitFor(store, me);
    EXPECT_EQ(store.profile(me)->all->gameCount(), 2U);
    // Again: September is complete, only October (still being played) is asked for.
    server.asked.clear();
    server.pages["/pub/player/bob/games/2026/10/pgn"] =
        game("Bob", "y", "1. d4 Nf6") + game("Bob", "z", "1. Nf3 d5");
    store.sync(me);
    waitFor(store, me);
    EXPECT_FALSE(server.asked.contains(QStringLiteral("/pub/player/bob/games/2026/09/pgn")));
    EXPECT_TRUE(server.asked.contains(QStringLiteral("/pub/player/bob/games/2026/10/pgn")));
    EXPECT_EQ(store.profile(me)->all->gameCount(), 3U);
}

TEST_F(ProfilesTest, OverTheBoardGamesAndOtherProfiles) {
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString friendId = store.addProfile(QStringLiteral("Magnus"));
    ASSERT_EQ(store.profiles().size(), 2U);
    QFile pgn(folder.filePath(QStringLiteral("club.pgn")));
    ASSERT_TRUE(pgn.open(QIODevice::WriteOnly));
    pgn.write(game("Magnus", "Hikaru", "1. e4 e5") + game("Hikaru", "Magnus", "1. d4 d5"));
    pgn.close();
    store.addOtbGames(friendId, {pgn.fileName()});
    waitFor(store, friendId);
    const auto* profile = store.profile(friendId);
    ASSERT_NE(profile->all, nullptr);
    EXPECT_EQ(profile->all->gameCount(), 2U);
    ASSERT_EQ(profile->parts.count(QStringLiteral("otb")), 1U);

    // An account that doesn't exist: said, and nothing breaks.
    QSignalSpy failed(&store, &ProfileStore::failed);
    store.addAccount(friendId, AccountSite::Lichess, QStringLiteral("nobody"));
    waitFor(store, friendId);
    ASSERT_EQ(failed.size(), 1);
    EXPECT_TRUE(failed.front().at(1).toString().contains(u"no Lichess account"));
    EXPECT_EQ(store.profile(friendId)->all->gameCount(), 2U);

    // Everything is still there after a restart.
    const ProfileStore again(folder.path());
    ASSERT_EQ(again.profiles().size(), 2U);
    const auto* loaded = again.profile(friendId);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name, QStringLiteral("Magnus"));
    ASSERT_NE(loaded->all, nullptr);
    EXPECT_EQ(loaded->all->gameCount(), 2U);
}

TEST_F(ProfilesTest, ThePageShowsAProfileAndItsPartsAndTheExplorerUsesThem) {
    server.pages["/api/games/user/alice"] =
        game("alice", "bob", "1. e4 e5 2. Nf3") + game("carol", "alice", "1. d4 d5");
    server.pages["/api/user/alice"] =
        R"({"title":"FM","count":{"all":5},"perfs":{"blitz":{"games":120,"rating":2210,)"
        R"("prov":false},"rapid":{"games":40,"rating":2150,"prov":false}}})";
    const kiebitz::app::AppStyle style(*qApp);
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 720);
    view.show();
    const QString me = store.profiles()[0].id;
    store.addAccount(me, AccountSite::Lichess, QStringLiteral("alice"));
    waitFor(store, me);
    QFile pgn(folder.filePath(QStringLiteral("club.pgn")));
    ASSERT_TRUE(pgn.open(QIODevice::WriteOnly));
    pgn.write(game("alice", "Hikaru", "1. c4 e5"));
    pgn.close();
    store.addOtbGames(me, {pgn.fileName()});
    waitFor(store, me);
    QApplication::processEvents();

    // All three games; then only the over-the-board one.
    auto* games = view.findChild<QTableView*>(QStringLiteral("profileGames"));
    ASSERT_NE(games, nullptr);
    EXPECT_EQ(games->model()->rowCount(), 3);
    // A dropdown with all games, Lichess · alice and over the board.
    auto* part = view.findChild<kiebitz::app::MultiChoice*>(QStringLiteral("profilePart"));
    ASSERT_NE(part, nullptr);
    const auto choice = [&](const QString& value) {
        return part->menu()->findChild<QAction*>(QStringLiteral("choice:") + value);
    };
    ASSERT_NE(choice(QStringLiteral("lichess-alice")), nullptr);
    EXPECT_EQ(choice(QStringLiteral("lichess-alice"))->text(), QStringLiteral("Lichess · alice"));
    choice(QStringLiteral("otb"))->trigger();
    EXPECT_EQ(games->model()->rowCount(), 1);
    // Both ticked: that is all of them.
    choice(QStringLiteral("lichess-alice"))->trigger();
    EXPECT_TRUE(part->selected().isEmpty());
    EXPECT_EQ(part->text(), QStringLiteral("All games"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return games->model()->rowCount() == 3; }, 10000));
    // The overview counts alice's games: two downloaded, one over the board.
    choice(QStringLiteral("all"))->trigger();
    EXPECT_TRUE(part->selected().isEmpty());
    auto* total = view.findChild<QLabel*>(QStringLiteral("profileTotal"));
    ASSERT_NE(total, nullptr);
    EXPECT_TRUE(QTest::qWaitFor([&] { return total->text().startsWith(u"3 games"); }, 5000))
        << total->text().toStdString();
    store.addProfile(QStringLiteral("Maximilian Alexander von Habsburg-Lothringen"));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QTest::qWait(100);
        view.grab().save(QDir(dir).filePath(QStringLiteral("profiles.png")));
    }

    // The explorer offers the profile and its parts.
    kiebitz::app::DatabaseManager databases(folder.filePath(QStringLiteral("dbs")));
    kiebitz::app::ExplorerPanel explorer(databases, store, repertoires, lichess, style);
    auto* source = explorer.findChild<QComboBox*>(QStringLiteral("explorerSource"));
    ASSERT_NE(source, nullptr);
    EXPECT_GE(source->findData(QStringLiteral("profile:") + me), 0);
    EXPECT_EQ(source->findData(QStringLiteral("profile:") + me + QStringLiteral(":otb")), -1);
    // The profile is in the list once; its parts are in the Filter tab.
    source->setCurrentIndex(source->findData(QStringLiteral("profile:") + me));
    emit source->activated(source->currentIndex());
    const kiebitz::core::Game start;
    QSignalSpy updated(&explorer, &kiebitz::app::ExplorerPanel::updated);
    explorer.setPosition(&start, kiebitz::core::Game::root(), kiebitz::core::Color::White);
    ASSERT_TRUE(updated.wait(5000));
    EXPECT_EQ(explorer.moveTexts().size(), 3); // e4, d4, c4

    // Filter: only the games where the profile (alice) had White: e4 and c4.
    QTest::mouseClick(explorer.findChild<QPushButton*>(QStringLiteral("explorerShowFilter")),
                      Qt::LeftButton);
    auto* asWhite = explorer.findChild<QPushButton*>(QStringLiteral("explorerSide1"));
    ASSERT_NE(asWhite, nullptr);
    EXPECT_TRUE(asWhite->isVisibleTo(&explorer)); // a profile: the colour can be chosen
    asWhite->click();
    ASSERT_TRUE(updated.wait(5000));
    EXPECT_EQ(explorer.moveTexts().size(), 2);
    EXPECT_TRUE(explorer.findChild<QPushButton*>(QStringLiteral("explorerShowFilter"))
                    ->text()
                    .contains(u"•"));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        explorer.resize(560, 320);
        explorer.show();
        QTest::qWait(100);
        explorer.grab().save(QDir(dir).filePath(QStringLiteral("explorer-filter.png")));
    }
    explorer.findChild<QPushButton*>(QStringLiteral("explorerFilterReset"))->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return explorer.moveTexts().size() == 3; }, 5000));

    // Games from: only the over-the-board one (c4).
    auto* from = explorer.findChild<QComboBox*>(QStringLiteral("explorerPart"));
    ASSERT_NE(from, nullptr);
    from->setCurrentIndex(from->findData(QStringLiteral("otb")));
    emit from->activated(from->currentIndex());
    ASSERT_TRUE(QTest::qWaitFor([&] { return explorer.moveTexts().size() == 1; }, 5000));
    EXPECT_TRUE(explorer.moveTexts().front().startsWith(u"c4"));
    // A player's name, for one colour: carol had White once (1. d4).
    explorer.findChild<QPushButton*>(QStringLiteral("explorerFilterReset"))->click();
    explorer.findChild<QLineEdit*>(QStringLiteral("explorerWhite"))->setText(QStringLiteral("CAR"));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const QStringList moves = explorer.moveTexts();
            return moves.size() == 1 && moves.front().startsWith(u"d4");
        },
        5000));
    explorer.findChild<QPushButton*>(QStringLiteral("explorerFilterReset"))->click();
}

namespace {

/// Two players as Lichess's search of FIDE's rating list answers them.
const char* const kFideAnswer =
    R"([{"id":99,"name":"Sanico, Magnus Carlsen","federation":"PHI","year":2001,)"
    R"("standard":1500,"gender":"M"},)"
    R"({"id":1503014,"name":"Carlsen, Magnus","federation":"NOR","year":1990,"title":"GM",)"
    R"("standard":2823,"rapid":2801,"blitz":2860,"gender":"M"}])";

} // namespace

TEST(Fide, TheLinkIsLichesssPageWithTheNameAsLichessWritesIt) {
    kiebitz::app::FidePlayer player;
    player.id = 1503014;
    player.name = QStringLiteral("Carlsen, Magnus");
    EXPECT_EQ(player.profileUrl(),
              QStringLiteral("https://lichess.org/fide/1503014/Carlsen_Magnus"));
    player.id = 556001355;
    player.name = QStringLiteral("Støren, Vetle  Andrè");
    EXPECT_EQ(player.profileUrl(),
              QStringLiteral("https://lichess.org/fide/556001355/Stren_Vetle_Andre"));
}

TEST(Fide, ReadsTheSearchAndRanksTheClosestNameFirst) {
    const auto players = kiebitz::app::parseFideSearch(kFideAnswer);
    ASSERT_EQ(players.size(), 2U);
    EXPECT_EQ(players[1].id, 1503014);
    EXPECT_EQ(players[1].name, QStringLiteral("Carlsen, Magnus"));
    EXPECT_EQ(players[1].title, QStringLiteral("GM"));
    EXPECT_EQ(players[1].federation, QStringLiteral("NOR"));
    EXPECT_EQ(players[1].standard, 2823);
    EXPECT_EQ(players[1].blitz, 2860);
    EXPECT_EQ(players[1].born, 1990);
    EXPECT_EQ(players[0].title, QString()); // no title, no rapid rating
    EXPECT_EQ(players[0].rapid, 0);
    EXPECT_TRUE(kiebitz::app::parseFideSearch("not json").empty());
    // "Magnus Carlsen" fits Carlsen, Magnus exactly; Sanico has a third name.
    const auto ranked = kiebitz::app::rankFideMatches(players, QStringLiteral("magnus CARLSEN"));
    ASSERT_EQ(ranked.size(), 2U);
    EXPECT_EQ(ranked[0].id, 1503014);
    EXPECT_EQ(kiebitz::app::rankFideMatches(players, QStringLiteral("Hikaru Nakamura")).size(), 0U);
}

TEST_F(ProfilesTest, LongAccountNamesNeverMoveTheCardsSideways) {
    const QString name = QStringLiteral("AVeryLongUsernameThatDoesNotFitAtAll");
    server.pages["/api/games/user/AVeryLongUsernameThatDoesNotFitAtAll"] =
        game("AVeryLongUsernameThatDoesNotFitAtAll", "bob", "1. e4");
    server.pages["/api/user/AVeryLongUsernameThatDoesNotFitAtAll"] =
        R"({"title":"GM","count":{"all":12345678},"perfs":{"blitz":{"games":120,)"
        R"("rating":2210,"prov":false}}})";
    const kiebitz::app::AppStyle style(*qApp);
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 400); // short: the list scrolls, with its scroll bar
    view.show();
    const QString me = store.profiles()[0].id;
    store.addAccount(me, AccountSite::Lichess, name);
    waitFor(store, me);
    auto* scroll = view.findChild<QScrollArea*>(QStringLiteral("profileAccounts"));
    ASSERT_NE(scroll, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        QApplication::processEvents();
        return scroll->widget()->width() == scroll->viewport()->width();
    }));
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
    // The name is cut short, in full as its tooltip, inside the list.
    const auto nameLabel = [&]() -> const QLabel* {
        for (const auto* label : scroll->widget()->findChildren<QLabel*>()) {
            if (label->toolTip() == name && label->isVisible()) {
                return label;
            }
        }
        return nullptr;
    };
    ASSERT_TRUE(QTest::qWaitFor([&] {
        const QLabel* label = nameLabel();
        return label != nullptr && label->mapTo(scroll->widget(), label->rect().topRight()).x() <
                                       scroll->viewport()->width();
    })) << "the name reaches past the list";
    EXPECT_TRUE(nameLabel()->text().endsWith(QStringLiteral("…")));
    EXPECT_EQ(scroll->widget()->width(), scroll->viewport()->width());
}

TEST_F(ProfilesTest, AProfileFindsItsFidePlayerAndCanPickAnother) {
    server.pages["/api/fide/player"] = kFideAnswer;
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Magnus Carlsen"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return store.profile(id)->fideLooked; }, 5000));
    EXPECT_TRUE(server.asked.back().startsWith(u"/api/fide/player?q=Magnus"));
    ASSERT_TRUE(store.profile(id)->fide.has_value());
    EXPECT_EQ(store.profile(id)->fide->id, 1503014);
    EXPECT_EQ(store.profile(id)->fideCandidates.size(), 2U);

    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        const kiebitz::app::AppStyle style(*qApp);
        kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
        kiebitz::app::Library library;
        kiebitz::app::RepertoireStore repertoires(library);
        kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
        view.resize(1180, 720);
        view.show();
        view.select(id);
        QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileFide")),
                          Qt::LeftButton);
        QTest::qWait(150);
        for (QWidget* top : QApplication::topLevelWidgets()) {
            if (top->objectName() == u"fideCard" && top->isVisible()) {
                // Asked once: is this them? The other players stay hidden until "No".
                const auto* others = top->findChild<QWidget*>(QStringLiteral("fideOthers"));
                const auto* found = top->findChild<QWidget*>(QStringLiteral("fidePlayer"));
                EXPECT_FALSE(others->isVisible());
                EXPECT_NE(top->findChild<QPushButton*>(QStringLiteral("fideYes")), nullptr);
                top->grab().save(QDir(dir).filePath(QStringLiteral("fide-card.png")));
                top->findChild<QPushButton*>(QStringLiteral("fideNo"))->click();
                EXPECT_TRUE(others->isVisible());
                EXPECT_FALSE(found->isVisible()); // not them: gone
                QTest::qWait(100);
                top->grab().save(QDir(dir).filePath(QStringLiteral("fide-card-others.png")));
                top->close();
            }
        }
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-fide.png")));
    }
    // Not them: the other one; then none at all. A lookup doesn't undo a choice.
    store.chooseFide(id, 99);
    EXPECT_EQ(store.profile(id)->fide->id, 99);
    store.chooseFide(id, 0);
    EXPECT_FALSE(store.profile(id)->fide.has_value());
    store.lookUpFide(id);
    QTest::qWait(300);
    EXPECT_FALSE(store.profile(id)->fide.has_value());

    // A one-word name isn't looked up ("Me", "Magnus" would find anyone).
    const int asked = static_cast<int>(server.asked.size());
    store.addProfile(QStringLiteral("Magnus"));
    QTest::qWait(200);
    EXPECT_EQ(server.asked.size(), asked);
}

TEST_F(ProfilesTest, WhenFideDoesNotAnswerItSaysSoAndCanTryAgain) {
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url()); // no FIDE page: an error
    const QString id = store.addProfile(QStringLiteral("Magnus Carlsen"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !store.profile(id)->fideError.isEmpty(); }, 5000));
    EXPECT_FALSE(store.profile(id)->fideLooked);    // so it is tried again next time
    server.pages["/api/fide/player"] = kFideAnswer; // FIDE is back
    store.lookUpFide(id);
    ASSERT_TRUE(QTest::qWaitFor([&] { return store.profile(id)->fideLooked; }, 5000));
    EXPECT_TRUE(store.profile(id)->fideError.isEmpty());
    EXPECT_EQ(store.profile(id)->fide->id, 1503014);
}

TEST_F(ProfilesTest, SigningInToLichessKeepsTheTokenAndWhoItIs) {
    using kiebitz::app::LichessLogin;
    server.pages["/api/token"] = R"({"token_type":"Bearer","access_token":"lio_secret"})";
    server.pages["/api/account"] = R"({"id":"alice","username":"Alice"})";
    FakeKeychain keychain;
    const LichessLogin::Storage storage = keychain.storage();
    LichessLogin login(storage);
    login.setServer(server.url());
    // The "browser": Lichess sends it back to the address the sign-in page names.
    QUrl opened;
    QUrl back; // where Lichess sends the browser
    QNetworkAccessManager browser;
    QList<QUrl> landed; // where the browser ended up
    login.setOpenUrl([&](const QUrl& url) {
        opened = url;
        const QUrlQuery query(url);
        back = QUrl(query.queryItemValue(QStringLiteral("redirect_uri")));
        back.setQuery(QStringLiteral("code=abc&state=") +
                      query.queryItemValue(QStringLiteral("state")));
        QNetworkReply* reply = browser.get(QNetworkRequest(back));
        QObject::connect(reply, &QNetworkReply::finished, reply, [&landed, reply] {
            landed << reply->url();
            reply->deleteLater();
        });
    });
    EXPECT_FALSE(login.signedIn());
    const QSignalSpy front(&login, &LichessLogin::signedInNow);
    login.signIn();
    EXPECT_EQ(opened.path(), QStringLiteral("/oauth"));
    EXPECT_EQ(QUrlQuery(opened).queryItemValue(QStringLiteral("code_challenge_method")),
              QStringLiteral("S256"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return login.signedIn(); }, 5000));
    EXPECT_EQ(login.usernames(), QStringList{QStringLiteral("Alice")});
    EXPECT_TRUE(login.signedIn(QStringLiteral("alice"))); // any case
    EXPECT_EQ(front.count(), 1);                          // Kiebitz comes back to the front
    EXPECT_EQ(login.token(), QByteArray("lio_secret"));
    EXPECT_EQ(keychain.tokens[QStringLiteral("alice")], QByteArray("lio_secret"));
    EXPECT_FALSE(login.busy());
    // The browser went on to the account's page on Lichess, not a page from this Mac.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !landed.isEmpty(); }, 5000));
    EXPECT_EQ(landed.front().path(), QStringLiteral("/@/Alice"));
    // A reload of the address it came back to: still answered (on to Lichess), not refused.
    QNetworkReply* reload = browser.get(QNetworkRequest(back));
    ASSERT_TRUE(QTest::qWaitFor([&] { return reload->isFinished(); }, 5000));
    EXPECT_NE(reload->error(), QNetworkReply::ConnectionRefusedError);
    EXPECT_EQ(reload->url().port(), server.url().port()); // sent on to "Lichess"
    reload->deleteLater();
    // The token request had the code and the verifier; the account request the token.
    const auto sent = [&](const char* path) {
        for (const QByteArray& request : server.requests) {
            if (request.split(' ').value(1).startsWith(path)) {
                return request;
            }
        }
        return QByteArray();
    };
    EXPECT_TRUE(sent("/api/token").startsWith("POST"));
    EXPECT_TRUE(sent("/api/token").contains("code=abc"));
    EXPECT_TRUE(sent("/api/token").contains("code_verifier="));
    EXPECT_TRUE(sent("/api/account").contains("Bearer lio_secret"));

    // A second account, signed in after the first, from its card: Lichess is asked for that
    // account. Both are kept, each with its token.
    server.pages["/api/token"] = R"({"token_type":"Bearer","access_token":"lio_other"})";
    server.pages["/api/account"] = R"({"id":"bob","username":"Bob"})";
    login.signIn(QStringLiteral("Bob"));
    EXPECT_EQ(QUrlQuery(opened).queryItemValue(QStringLiteral("username")), QStringLiteral("Bob"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return login.signedIn(QStringLiteral("Bob")); }, 5000));
    EXPECT_EQ(login.usernames(), (QStringList{QStringLiteral("Alice"), QStringLiteral("Bob")}));
    EXPECT_EQ(login.token(QStringLiteral("bob")), QByteArray("lio_other"));
    EXPECT_EQ(login.token(QStringLiteral("Alice")), QByteArray("lio_secret"));
    EXPECT_EQ(login.token(QStringLiteral("carol")), QByteArray()); // not signed in

    // Started again, both are still signed in; Alice signed out, her token is gone here and on
    // Lichess, Bob's stays.
    LichessLogin again(storage);
    again.setServer(server.url());
    EXPECT_EQ(again.usernames(), (QStringList{QStringLiteral("Alice"), QStringLiteral("Bob")}));
    EXPECT_EQ(again.token(QStringLiteral("Alice")), QByteArray("lio_secret"));
    again.signOut(QStringLiteral("alice"));
    EXPECT_EQ(again.usernames(), QStringList{QStringLiteral("Bob")});
    EXPECT_FALSE(keychain.tokens.contains(QStringLiteral("alice")));
    EXPECT_EQ(again.token(), QByteArray("lio_other")); // the explorer uses Bob's now
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return std::any_of(
                server.requests.begin(), server.requests.end(), [](const QByteArray& r) {
                    return r.startsWith("DELETE /api/token") && r.contains("Bearer lio_secret");
                });
        },
        5000));
}

TEST_F(ProfilesTest, LichessLoginsAreKeptInAFileOnlyYouCanRead) {
    using kiebitz::app::LichessLogin;
    const QString path = folder.filePath(QStringLiteral("support/lichess-logins.json"));
    const LichessLogin::Storage storage = LichessLogin::privateFile(path);
    EXPECT_TRUE(storage.loadUsernames().isEmpty());
    storage.saveToken(QStringLiteral("CatalanSimp"), "lio_one");
    storage.saveToken(QStringLiteral("ConstantlyTilting"), "lio_two");
    storage.saveToken(QStringLiteral("catalansimp"), "lio_new"); // the same account, any case
    EXPECT_EQ(storage.loadUsernames(),
              (QStringList{QStringLiteral("ConstantlyTilting"), QStringLiteral("catalansimp")}));
    EXPECT_EQ(storage.loadToken(QStringLiteral("CATALANSIMP")), QByteArray("lio_new"));
    EXPECT_EQ(storage.loadToken(QStringLiteral("ConstantlyTilting")), QByteArray("lio_two"));
    EXPECT_FALSE(storage.loadToken(QStringLiteral("nobody")).has_value());
    // Only you can read it.
    EXPECT_EQ(QFile::permissions(path) & ~(QFileDevice::ReadUser | QFileDevice::WriteUser),
              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    // Signed out: gone from the file; the other stays.
    storage.removeToken(QStringLiteral("ConstantlyTilting"));
    EXPECT_EQ(storage.loadUsernames(), QStringList{QStringLiteral("catalansimp")});
    // Read by a login as it starts: signed in, with the token.
    LichessLogin login(storage);
    EXPECT_TRUE(login.signedIn(QStringLiteral("CatalanSimp")));
    EXPECT_EQ(login.token(QStringLiteral("CatalanSimp")), QByteArray("lio_new"));
}

TEST_F(ProfilesTest, ALichessSignInYouTurnDownChangesNothing) {
    using kiebitz::app::LichessLogin;
    LichessLogin login(LichessLogin::Storage{});
    login.setServer(server.url());
    QNetworkAccessManager browser;
    login.setOpenUrl([&](const QUrl& url) {
        const QUrlQuery query(url);
        QUrl back(query.queryItemValue(QStringLiteral("redirect_uri")));
        back.setQuery(QStringLiteral("error=access_denied&state=") +
                      query.queryItemValue(QStringLiteral("state")));
        const QNetworkReply* reply = browser.get(QNetworkRequest(back));
        QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    });
    const QSignalSpy failed(&login, &LichessLogin::failed);
    login.signIn();
    EXPECT_TRUE(login.busy());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !login.busy(); }, 5000));
    EXPECT_FALSE(login.signedIn());
    EXPECT_EQ(failed.count(), 0);
}

TEST(LichessExplorer, AsksForTheFilterLichessKnows) {
    using kiebitz::app::LichessExplorer;
    const kiebitz::core::Position position;
    const auto e4 = position.parseSan("e4").value();
    kiebitz::gamedb::ExploreFilter filter;
    filter.minRating = 1700;
    filter.maxRating = 2100;
    filter.fromYear = 2020;
    filter.speeds =
        static_cast<std::uint8_t>((1U << static_cast<unsigned>(kiebitz::gamedb::Speed::Blitz)) |
                                  (1U << static_cast<unsigned>(kiebitz::gamedb::Speed::Rapid)));
    const QUrl url = LichessExplorer::url(QUrl(QStringLiteral("https://x")),
                                          LichessExplorer::Kind::Lichess, {e4}, filter);
    const QUrlQuery query(url);
    EXPECT_EQ(url.path(), QStringLiteral("/lichess"));
    EXPECT_EQ(query.queryItemValue(QStringLiteral("play")), QStringLiteral("e2e4"));
    EXPECT_EQ(query.queryItemValue(QStringLiteral("speeds")), QStringLiteral("blitz,rapid"));
    // 1600–1800 holds 1700; up to 2000–2200, which holds 2100.
    EXPECT_EQ(query.queryItemValue(QStringLiteral("ratings")), QStringLiteral("1600,1800,2000"));
    EXPECT_EQ(query.queryItemValue(QStringLiteral("since")), QStringLiteral("2020-01"));
    EXPECT_FALSE(query.hasQueryItem(QStringLiteral("until")));
    // The masters: only the years.
    const QUrlQuery masters(LichessExplorer::url(QUrl(QStringLiteral("https://x")),
                                                 LichessExplorer::Kind::Masters, {}, filter));
    EXPECT_EQ(masters.queryItemValue(QStringLiteral("since")), QStringLiteral("2020"));
    EXPECT_FALSE(masters.hasQueryItem(QStringLiteral("speeds")));
}

TEST_F(ProfilesTest, TheExplorerShowsLichessOnceSignedIn) {
    using kiebitz::app::LichessLogin;
    server.pages["/masters"] =
        R"({"white":60,"draws":30,"black":10,"moves":[)"
        R"({"uci":"e2e4","san":"e4","averageRating":2410,"white":40,"draws":20,"black":5},)"
        R"({"uci":"d2d4","san":"d4","averageRating":2430,"white":20,"draws":10,"black":5}],)"
        R"("topGames":[{"uci":"e2e4","id":"abcd1234","winner":"white",)"
        R"("white":{"name":"Carlsen, M.","rating":2863},"black":{"name":"Caruana, F.",)"
        R"("rating":2828},"year":2019,"month":"2019-04"}]})";
    server.pages["/masters/pgn/abcd1234"] =
        "[White \"Carlsen, M.\"]\n[Black \"Caruana, F.\"]\n[Result \"1-0\"]\n\n1. e4 e5 1-0\n";
    const kiebitz::app::AppStyle style(*qApp);
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    kiebitz::app::DatabaseManager databases(folder.filePath(QStringLiteral("databases")));
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    // Signed out: the explorer says what to do.
    FakeKeychain keychain;
    const LichessLogin::Storage storage = keychain.storage();
    LichessLogin signedOut(storage);
    kiebitz::app::ExplorerPanel offline(databases, store, repertoires, signedOut, style);
    offline.show();
    auto* offlineSource = offline.findChild<QComboBox*>(QStringLiteral("explorerSource"));
    offlineSource->setCurrentIndex(offlineSource->findData(QStringLiteral("lichess:masters")));
    emit offlineSource->activated(offlineSource->currentIndex());
    EXPECT_TRUE(offline.findChild<QWidget*>(QStringLiteral("explorerSignIn"))->isVisible());

    // Signed in: the masters' moves and top games, asked for with the token.
    keychain.tokens[QStringLiteral("alice")] = QByteArray("lio_secret");
    keychain.names = QStringList{QStringLiteral("Alice")};
    LichessLogin login(storage);
    kiebitz::app::ExplorerPanel explorer(databases, store, repertoires, login, style);
    explorer.lichessExplorer().setServers(server.url(), server.url());
    explorer.show();
    auto* source = explorer.findChild<QComboBox*>(QStringLiteral("explorerSource"));
    source->setCurrentIndex(source->findData(QStringLiteral("lichess:masters")));
    emit source->activated(source->currentIndex());
    ASSERT_TRUE(QTest::qWaitFor([&] { return explorer.moveTexts().size() == 2; }, 5000));
    EXPECT_EQ(explorer.moveTexts().front(), QStringLiteral("e4 65 2410"));
    EXPECT_FALSE(explorer.findChild<QWidget*>(QStringLiteral("explorerSignIn"))->isVisible());
    EXPECT_TRUE(std::any_of(server.requests.begin(), server.requests.end(), [](const auto& r) {
        return r.startsWith("GET /masters") && r.contains("Bearer lio_secret");
    }));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        explorer.resize(460, 520);
        QTest::qWait(100);
        explorer.grab().save(QDir(dir).filePath(QStringLiteral("explorer-masters.png")));
    }
    // A top game opens whole.
    auto* games = explorer.findChild<QTreeWidget*>(QStringLiteral("explorerGames"));
    ASSERT_EQ(games->topLevelItemCount(), 1);
    EXPECT_EQ(games->topLevelItem(0)->text(0), QStringLiteral("Carlsen, M. (2863)"));
    QSignalSpy opened(&explorer, &kiebitz::app::ExplorerPanel::openGameRequested);
    emit games->itemDoubleClicked(games->topLevelItem(0), 0);
    EXPECT_TRUE(opened.wait(5000));
    QSettings().remove(QStringLiteral("explorer/source")); // other tests start without a choice
}

TEST_F(ProfilesTest, AfterTooManyRequestsLichessIsLeftAloneForAMinute) {
    using kiebitz::app::LichessExplorer;
    namespace lichess = kiebitz::app::lichess;
    lichess::endPause();
    // A download Lichess answers with 429: it says to wait, and the minute's pause starts.
    server.statuses["/api/games/user/alice"] = 429;
    server.pages["/api/user/alice"] = R"({"perfs":{}})";
    ProfileStore store(folder.path());
    store.setServers(server.url(), server.url(), server.url());
    const QString me = store.profiles()[0].id;
    QSignalSpy failed(&store, &ProfileStore::failed);
    store.addAccount(me, AccountSite::Lichess, QStringLiteral("alice"));
    waitFor(store, me);
    ASSERT_GE(failed.count(), 1);
    EXPECT_TRUE(failed.front().at(1).toString().contains(u"wait a minute"));
    EXPECT_GT(lichess::pauseLeftMs(), 50'000);
    // Kiebitz says who it is, and nothing about the user.
    EXPECT_TRUE(std::any_of(server.requests.begin(), server.requests.end(),
                            [](const auto& r) { return r.contains("User-Agent: Kiebitz/"); }));

    // Meanwhile the explorer doesn't ask Lichess at all: it says to wait.
    FakeKeychain keychain;
    kiebitz::app::LichessLogin login(keychain.storage());
    LichessExplorer explorer(login);
    explorer.setServers(server.url(), server.url());
    const qsizetype asked = server.asked.size();
    std::optional<LichessExplorer::Answer> answer;
    explorer.ask(LichessExplorer::Kind::Masters, {}, {},
                 [&](const LichessExplorer::Answer& a) { answer = a; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return answer.has_value(); }, 5000));
    EXPECT_TRUE(answer->error.contains(u"slow down"));
    EXPECT_EQ(server.asked.size(), asked);

    // And a second download waits too, without asking.
    store.sync(me);
    waitFor(store, me);
    EXPECT_EQ(server.asked.size(), asked);
    lichess::endPause(); // the other tests in this process
}

TEST(LiveNetwork, LichessKnowsFidePlayersByName) {
    if (qEnvironmentVariableIsEmpty("KIEBITZ_LIVE_NETWORK")) {
        GTEST_SKIP() << "set KIEBITZ_LIVE_NETWORK=1 to run";
    }
    const QTemporaryDir folder;
    ProfileStore store(folder.path()); // the real servers
    const QString id = store.addProfile(QStringLiteral("Magnus Carlsen"));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return store.profile(id)->fideLooked || !store.profile(id)->fideError.isEmpty(); },
        15000));
    EXPECT_TRUE(store.profile(id)->fideError.isEmpty())
        << store.profile(id)->fideError.toStdString();
    ASSERT_TRUE(store.profile(id)->fide.has_value());
    EXPECT_EQ(store.profile(id)->fide->id, 1503014);
    EXPECT_EQ(store.profile(id)->fide->federation, QStringLiteral("NOR"));
    EXPECT_GT(store.profile(id)->fide->standard, 2700);
}

TEST_F(ProfilesTest, TheRepertoireTabShowsWhereGamesLeftTheBook) {
    // A White repertoire: 1. e4 e5 2. Nf3 Nc6 3. Bb5; against 1... c5, 2. Nf3.
    const QString books = folder.filePath(QStringLiteral("books"));
    QDir().mkpath(books);
    QFile book(QDir(books).filePath(QStringLiteral("white.pgn")));
    ASSERT_TRUE(book.open(QIODevice::WriteOnly));
    book.write("[FileType \"Repertoire\"]\n[Orientation \"white\"]\n\n"
               "1. e4 e5 (1... c5 2. Nf3) 2. Nf3 Nc6 3. Bb5 *\n");
    book.close();
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library); // reads the files once they're found
    QSignalSpy scanned(&library, &kiebitz::app::Library::scanned);
    library.setFolder(books);
    ASSERT_TRUE(scanned.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return repertoires.get(kiebitz::core::Color::White) != nullptr; }, 5000));

    // Alice's over-the-board games: 2. Bc4 twice (she left the book), 1... e6 once (they did),
    // and one with Black (no Black repertoire: not checked).
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    // Your own profile (the tab is only there): you are Alice.
    const QString id = store.profiles()[0].id;
    store.renameProfile(id, QStringLiteral("Alice Smith"));
    QFile pgn(folder.filePath(QStringLiteral("club.pgn")));
    ASSERT_TRUE(pgn.open(QIODevice::WriteOnly));
    pgn.write(game("Smith, Alice", "Bob", "1. e4 e5 2. Bc4 Nf6"));
    pgn.write(game("Smith, Alice", "Carol", "1. e4 e5 2. Bc4 Bc5"));
    pgn.write(game("Smith, Alice", "Dave", "1. e4 e6 2. d4"));
    pgn.write(game("Eve", "Smith, Alice", "1. d4 d5"));
    pgn.write(game("Smith, Alice", "Frank", "1. c4 e5")); // she left it on her first move
    pgn.close();
    store.addOtbGames(id, {pgn.fileName()});
    waitFor(store, id);

    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 720);
    view.show();
    view.select(id);
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileShowRepertoire")),
                      Qt::LeftButton);
    // Done (the profile may still be refreshing, which checks again).
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return view.theorySummary().contains(u"games checked") && store.status(id).isEmpty();
        },
        5000))
        << view.theorySummary().toStdString();
    QTest::qWait(200);
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return view.theorySummary().contains(u"games checked"); }, 5000));
    // Under their openings (the names come in the background).
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !view.theoryRows().join(u'|').contains(u"Other openings"); }, 20000));
    QStringList rows = view.theoryRows();
    // Every opening starts closed: you open the one you want.
    if (const auto* theory = view.findChild<QTreeWidget*>(QStringLiteral("profileTheory"))) {
        for (int i = 0; i < theory->topLevelItemCount(); ++i) {
            EXPECT_FALSE(theory->topLevelItem(i)->isExpanded()) << i;
        }
    }
    rows.sort(); // openings with as many games come in any order
    EXPECT_EQ(rows, (QStringList{QStringLiteral("Bishop's Opening: 2. Bc4  You  2  2. Nf3"),
                                 QStringLiteral("English Opening: 1. c4  You  1  1. e4"),
                                 QStringLiteral("French Defense: 1… e6  They  1  covers 1… e5, "
                                                "1… c5")}));
    // The line to the move, as a score sheet writes it.
    const auto* theory = view.findChild<QTreeWidget*>(QStringLiteral("profileTheory"));
    for (int i = 0; i < theory->topLevelItemCount(); ++i) {
        if (theory->topLevelItem(i)->text(0) == u"Bishop's Opening") {
            EXPECT_EQ(theory->topLevelItem(i)->child(0)->toolTip(0), QStringLiteral("1. e4 e5"));
        }
    }
    EXPECT_TRUE(view.theorySummary().startsWith(u"4 games checked")) // the Black one isn't
        << view.theorySummary().toStdString();
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QTest::qWait(100);
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-repertoire.png")));
    }
    // The speed filter at the top counts here too: her games have no time control (classical).
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileSpeed:blitz")),
                      Qt::LeftButton);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return view.theorySummary().startsWith(u"No games at the speeds"); }, 5000))
        << view.theorySummary().toStdString();
    EXPECT_TRUE(view.theoryRows().isEmpty());
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileSpeed:classical")),
                      Qt::LeftButton);
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return view.theorySummary().startsWith(u"4 games checked"); }, 5000));
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileSpeed:all")),
                      Qt::LeftButton);
    ASSERT_TRUE(
        QTest::qWaitFor([&] { return view.theorySummary().startsWith(u"4 games checked"); }, 5000));
    // Only where they left it.
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileTheoryKind:opponent")),
                      Qt::LeftButton);
    EXPECT_EQ(view.theoryRows().size(), 1);
    // A game of the group, double-clicked: it opens at 2... (after the move that left the book).
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileTheoryKind:all")),
                      Qt::LeftButton);
    auto* tree = view.findChild<QTreeWidget*>(QStringLiteral("profileTheory"));
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return tree->topLevelItemCount() == 3; }, 5000));
    QTreeWidgetItem* bc4 = tree->topLevelItem(0)->child(0);
    ASSERT_EQ(bc4->childCount(), 2);
    QSignalSpy opened(&view, &kiebitz::app::ProfilesView::openGameAtRequested);
    emit tree->itemDoubleClicked(bc4->child(0), 0);
    ASSERT_TRUE(opened.count() == 1 || opened.wait(5000)); // found in its PGN first
    EXPECT_EQ(opened.front().at(1).toInt(), 3);            // 1. e4 e5 2. Bc4
    // The move itself: the repertoire, there.
    const QSignalSpy book2(&view, &kiebitz::app::ProfilesView::repertoireRequested);
    emit tree->itemDoubleClicked(bc4, 0);
    EXPECT_EQ(book2.count(), 1);

    // Saved: another view (as after a restart) shows it at once, without checking again.
    kiebitz::app::ProfilesView second(store, lichess, repertoires, style);
    second.resize(1180, 720);
    second.show();
    second.select(id);
    const QSignalSpy shown(&second, &kiebitz::app::ProfilesView::theoryChecked);
    QTest::mouseClick(second.findChild<QPushButton*>(QStringLiteral("profileShowRepertoire")),
                      Qt::LeftButton);
    EXPECT_EQ(shown.count(), 1); // there and then, not after a scan
    EXPECT_TRUE(second.theorySummary().startsWith(u"4 games checked"));
    EXPECT_FALSE(second.theorySummary().contains(u"Updating"));
    // A new game: checked again (the old result shows meanwhile).
    QFile more(folder.filePath(QStringLiteral("more.pgn")));
    ASSERT_TRUE(more.open(QIODevice::WriteOnly));
    more.write(game("Smith, Alice", "Gina", "1. e4 e5 2. Bc4 Bc5"));
    more.close();
    store.addOtbGames(id, {more.fileName()});
    waitFor(store, id);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return second.theorySummary().startsWith(u"5 games checked"); }, 5000))
        << second.theorySummary().toStdString();

    // It is new: the games from before were all seen when the profile was first shown.
    auto* tab = second.findChild<QPushButton*>(QStringLiteral("profileShowRepertoire"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return tab->text() == u"Repertoire · 1 new"; }, 5000))
        << tab->text().toStdString();
    QTest::mouseClick(second.findChild<QPushButton*>(QStringLiteral("profileTheoryKind:new")),
                      Qt::LeftButton);
    EXPECT_TRUE(second.theorySummary().startsWith(u"1 new game since"))
        << second.theorySummary().toStdString();
    const auto* newTree = second.findChild<QTreeWidget*>(QStringLiteral("profileTheory"));
    ASSERT_EQ(newTree->topLevelItemCount(), 1);
    EXPECT_EQ(newTree->topLevelItem(0)->text(0), QStringLiteral("Bishop's Opening"));
    EXPECT_EQ(newTree->topLevelItem(0)->text(2), QStringLiteral("1"));
    ASSERT_EQ(newTree->topLevelItem(0)->child(0)->childCount(), 1);
    EXPECT_TRUE(newTree->topLevelItem(0)->child(0)->child(0)->text(0).contains(u"Gina"));
    // Looked at: seen, also for the next view (as after a restart).
    auto* markSeen = second.findChild<QPushButton*>(QStringLiteral("profileMarkSeen"));
    ASSERT_TRUE(markSeen->isVisible());
    QTest::mouseClick(markSeen, Qt::LeftButton);
    ASSERT_TRUE(QTest::qWaitFor([&] { return tab->text() == u"Repertoire"; }, 5000));
    EXPECT_FALSE(markSeen->isVisible());
    EXPECT_EQ(newTree->topLevelItemCount(), 0);
    EXPECT_TRUE(second.theorySummary().startsWith(u"No new games"));
    kiebitz::app::ProfilesView third(store, lichess, repertoires, style);
    third.select(id);
    const auto* thirdTab = third.findChild<QPushButton*>(QStringLiteral("profileShowRepertoire"));
    QTest::qWait(300);
    EXPECT_EQ(thirdTab->text(), QStringLiteral("Repertoire"));
}

TEST_F(ProfilesTest, OverTheBoardGamesAreAddedOnceAndCanBeDeleted) {
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Alice Smith"));
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    // A file with a game twice, and another game.
    QFile pgn(folder.filePath(QStringLiteral("club.pgn")));
    ASSERT_TRUE(pgn.open(QIODevice::WriteOnly));
    pgn.write(game("Smith, Alice", "Bob", "1. e4 e5 2. Bc4 Nf6"));
    pgn.write(game("Smith, Alice", "Bob", "1. e4 { a comment } e5 2. Bc4 Nf6")); // the same game
    pgn.write(game("Carol", "Smith, Alice", "1. d4 d5"));
    pgn.close();
    store.addOtbGames(id, {pgn.fileName()});
    waitFor(store, id);
    ASSERT_EQ(added.size(), 1);
    EXPECT_EQ(added.front().at(1).toInt(), 2); // added
    EXPECT_EQ(added.front().at(2).toInt(), 1); // left out
    EXPECT_EQ(store.profile(id)->all->gameCount(), 2U);
    // The same file again: nothing new.
    store.addOtbGames(id, {pgn.fileName()});
    waitFor(store, id);
    ASSERT_EQ(added.size(), 2);
    EXPECT_EQ(added.back().at(1).toInt(), 0);
    EXPECT_EQ(added.back().at(2).toInt(), 3);
    EXPECT_EQ(store.profile(id)->all->gameCount(), 2U);

    // A duplicate already in the profile's files (from before this check): deleting one copy
    // keeps the other.
    const QString otb = folder.filePath(QStringLiteral("profiles/") + id + QStringLiteral("/otb"));
    QFile copy(QDir(otb).filePath(QStringLiteral("old copy.pgn")));
    ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    copy.write(game("Carol", "Smith, Alice", "1. d4 d5"));
    copy.close();
    QFile third(folder.filePath(QStringLiteral("third.pgn")));
    ASSERT_TRUE(third.open(QIODevice::WriteOnly));
    third.write(game("Smith, Alice", "Dave", "1. c4 c5"));
    third.close();
    store.addOtbGames(id, {third.fileName()}); // rebuilds with the old copy in it
    waitFor(store, id);
    // Over the board both copies are there; all games count it once.
    const auto otbGames = [&] {
        return store.profile(id)->parts.at(QStringLiteral("otb"));
    };
    ASSERT_EQ(otbGames()->gameCount(), 4U);
    EXPECT_EQ(store.profile(id)->all->gameCount(), 3U);
    // Carol's game, from the database: deleted once, there once more.
    const auto db = otbGames();
    std::optional<kiebitz::core::Game> carol;
    for (const auto& info : db->games(1, 10)) {
        if (info.white == "Carol") {
            carol = db->game(info.id);
        }
    }
    ASSERT_TRUE(carol.has_value());
    store.deleteGame(id, QString(), *carol);
    waitFor(store, id);
    EXPECT_EQ(otbGames()->gameCount(), 3U);
    store.deleteGame(id, QStringLiteral("otb"), *carol);
    waitFor(store, id);
    EXPECT_EQ(otbGames()->gameCount(), 2U);
    // Opened, a game is as its PGN has it: with its clocks and comments.
    QFile timed(folder.filePath(QStringLiteral("timed.pgn")));
    ASSERT_TRUE(timed.open(QIODevice::WriteOnly));
    timed.write("[White \"Smith, Alice\"]\n[Black \"Erin\"]\n[Date \"2026.01.01\"]\n"
                "[Result \"1-0\"]\n\n1. e4 { [%clk 1:29:58] } e5 { [%clk 1:29:50] } 1-0\n");
    timed.close();
    store.addOtbGames(id, {timed.fileName()});
    waitFor(store, id);
    std::optional<kiebitz::core::Game> fromDb;
    for (const auto& info : store.profile(id)->all->games(1, 10)) {
        if (info.black == "Erin") {
            fromDb = store.profile(id)->all->game(info.id);
        }
    }
    ASSERT_TRUE(fromDb.has_value());
    const auto firstComment = [](const kiebitz::core::Game& g) {
        return g.node(g.mainLine().at(1)).comment;
    };
    EXPECT_EQ(firstComment(*fromDb), ""); // the database keeps only the moves
    std::optional<kiebitz::core::Game> original;
    store.originalGame(id, QString(), *fromDb,
                       [&](const kiebitz::core::Game& game) { original = game; });
    ASSERT_TRUE(QTest::qWaitFor([&] { return original.has_value(); }, 5000));
    EXPECT_EQ(firstComment(*original), "[%clk 1:29:58]");

    // Gone for good: deleting it again says so.
    QSignalSpy failed(&store, &ProfileStore::failed);
    store.deleteGame(id, QString(), *carol);
    ASSERT_TRUE(failed.wait(5000));
    EXPECT_EQ(store.profile(id)->all->gameCount(), 3U); // Erin's game was added since
}

TEST(MessageDialog, AsksWithButtonsNamedForWhatTheyDo) {
    using kiebitz::app::MessageDialog;
    const kiebitz::app::AppStyle style(*qApp);
    MessageDialog question(QStringLiteral("Remove Account"),
                           QStringLiteral("Remove lsadkjföd and its games from this profile?"), {},
                           QStringLiteral("Remove"), MessageDialog::Action::Destructive, nullptr);
    auto* act = question.findChild<QPushButton*>(QStringLiteral("messageAction"));
    auto* cancel = question.findChild<QPushButton*>(QStringLiteral("messageCancel"));
    ASSERT_NE(act, nullptr);
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(act->text(), QStringLiteral("Remove"));
    EXPECT_EQ(act->property("role").toString(), QStringLiteral("danger"));
    EXPECT_TRUE(cancel->isDefault()); // Return doesn't remove anything
    MessageDialog note(QStringLiteral("Add Account"), QStringLiteral("There's no account."), {}, {},
                       MessageDialog::Action::Normal, nullptr);
    EXPECT_NE(note.findChild<QPushButton*>(QStringLiteral("messageOk")), nullptr);
    EXPECT_EQ(note.findChild<QPushButton*>(QStringLiteral("messageAction")), nullptr);
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        question.show();
        QTest::qWait(100);
        question.grab().save(QDir(dir).filePath(QStringLiteral("message.png")));
    }
}

TEST(Sounds, EverySetHasShortWavFilesAtTheVolumeChosen) {
    using kiebitz::app::Sounds;
    // The samples of a WAV the app wrote: after its header (format chunk of 16 bytes).
    const auto loudest = [](const QByteArray& wav) {
        int peak = 0;
        for (qsizetype i = 44; i + 1 < wav.size(); i += 2) {
            const auto sample = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(static_cast<std::uint8_t>(wav[i])) |
                static_cast<std::uint16_t>(static_cast<std::uint8_t>(wav[i + 1]) << 8U));
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        return peak;
    };
    ASSERT_EQ(Sounds::sets().front(), QStringLiteral("standard")); // the default
    for (const QString& set : Sounds::sets()) {
        for (const auto kind : {Sounds::Kind::Move, Sounds::Kind::Capture, Sounds::Kind::Check}) {
            const QByteArray wav = Sounds::wav(set, kind, 70);
            ASSERT_GT(wav.size(), 44) << set.toStdString();
            EXPECT_TRUE(wav.startsWith("RIFF"));
            EXPECT_EQ(wav.mid(8, 8), QByteArray("WAVEfmt "));
            EXPECT_EQ(wav.mid(36, 4), QByteArray("data"));
            EXPECT_LT(wav.size() - 44, 44100 * 2); // under a second (16-bit mono)
            // Softer at a lower volume, silent at none.
            EXPECT_GT(loudest(Sounds::wav(set, kind, 100)), loudest(Sounds::wav(set, kind, 40)));
            EXPECT_EQ(loudest(Sounds::wav(set, kind, 0)), 0);
        }
    }
    EXPECT_TRUE(Sounds::wav(QStringLiteral("none"), Sounds::Kind::Move, 70).isEmpty());
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QFile file(QDir(dir).filePath(QStringLiteral("capture.wav"))); // to check with afinfo
        if (file.open(QIODevice::WriteOnly)) {
            file.write(Sounds::wav(QStringLiteral("sfx"), Sounds::Kind::Capture, 70));
        }
    }
}

TEST(Icons, SidebarIconsSnapshot) {
    const kiebitz::app::AppStyle style(*qApp);
    const auto& c = style.colors();
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        // The sidebar's icons side by side, large, to see them as a set.
        QPixmap sheet(4 * 96, 96);
        sheet.fill(c.window);
        QPainter painter(&sheet);
        int x = 8;
        for (const auto kind : {kiebitz::app::IconKind::Person, kiebitz::app::IconKind::Engine,
                                kiebitz::app::IconKind::Gear, kiebitz::app::IconKind::Database}) {
            painter.drawPixmap(x, 8,
                               kiebitz::app::makeIcon(kind, c.text, c.dimmed, 80).pixmap(80, 80));
            x += 96;
        }
        painter.end();
        sheet.save(QDir(dir).filePath(QStringLiteral("icons.png")));
    }
    SUCCEED();
}

namespace {

/// A database like Lichess's broadcasts: games with the players' FIDE ids.
std::shared_ptr<const kiebitz::gamedb::Database> broadcastsDb(const QString& folder,
                                                              const std::string& pgn) {
    QDir().mkpath(folder);
    {
        auto writer = kiebitz::gamedb::DatabaseWriter::create(folder.toStdString(),
                                                              {"Lichess broadcasts", "", 0, 0});
        if (!writer.ok()) {
            return nullptr;
        }
        kiebitz::gamedb::GameSplitter splitter;
        const auto add = [&](std::string_view game) {
            (*writer)->add(kiebitz::gamedb::scanGame(game));
        };
        splitter.feed(pgn, add);
        splitter.finish(add);
        if (!(*writer)->finish().empty()) {
            return nullptr;
        }
    }
    auto db = kiebitz::gamedb::Database::open(folder.toStdString());
    return db.ok() ? std::shared_ptr<const kiebitz::gamedb::Database>(std::move(*db)) : nullptr;
}

} // namespace

TEST_F(ProfilesTest, EachDatabaseGivesThePlayersGamesAPartOfItsOwnAndAllCountsThemOnce) {
    server.pages["/api/fide/player"] = kFideAnswer;
    // Lichess's broadcasts: found by FIDE id.
    const auto broadcasts = broadcastsDb(
        folder.filePath(QStringLiteral("broadcasts")),
        "[White \"Carlsen, Magnus\"]\n[Black \"Caruana, Fabiano\"]\n[WhiteFideId \"1503014\"]\n"
        "[BlackFideId \"2020009\"]\n[Date \"2026.09.29\"]\n[Result \"1-0\"]\n\n"
        "1. e4 e5 2. Nf3 1-0\n\n"
        "[White \"Nakamura, Hikaru\"]\n[Black \"Carlsen, Magnus\"]\n[WhiteFideId \"2016192\"]\n"
        "[BlackFideId \"1503014\"]\n[Date \"2026.09.30\"]\n[Result \"0-1\"]\n\n"
        "1. d4 Nf6 0-1\n\n"
        "[White \"Someone, Else\"]\n[Black \"Another, One\"]\n[WhiteFideId \"5\"]\n"
        "[BlackFideId \"6\"]\n[Result \"*\"]\n\n1. c4 *\n");
    // A database without FIDE ids (Caissabase): found by name; the same game as in the
    // broadcasts once more, and a namesake who isn't him.
    const auto caissabase = broadcastsDb(
        folder.filePath(QStringLiteral("caissabase")),
        "[White \"Carlsen, Magnus\"]\n[Black \"Kasparov, Garry\"]\n[Date \"2004.03.18\"]\n"
        "[Result \"1/2-1/2\"]\n\n1. d4 d5 1/2-1/2\n\n"
        "[White \"Carlsen, Magnus\"]\n[Black \"Caruana, Fabiano\"]\n[Date \"2026.09.29\"]\n"
        "[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 1-0\n\n"
        "[White \"Carlsen, Henrik\"]\n[Black \"X\"]\n[Result \"1-0\"]\n\n1. e4 1-0\n");
    ASSERT_NE(broadcasts, nullptr);
    ASSERT_NE(caissabase, nullptr);
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    store.setGameSources([&] {
        return std::vector<std::pair<QString, std::shared_ptr<const kiebitz::gamedb::Database>>>{
            {QStringLiteral("Lichess broadcasts"), broadcasts},
            {QStringLiteral("Caissabase"), caissabase}};
    });
    const QString id = store.addProfile(QStringLiteral("Magnus Carlsen"));
    // The FIDE player found: two games from each database, the shared one once in all.
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const auto* profile = store.profile(id);
            return !store.busy(id) && profile->all && profile->all->gameCount() == 3;
        },
        10000));
    const auto* profile = store.profile(id);
    ASSERT_EQ(profile->sources.size(), 2U);
    EXPECT_EQ(profile->sources[0].second, QStringLiteral("Caissabase"));
    EXPECT_EQ(profile->sources[1].second, QStringLiteral("Lichess broadcasts"));
    EXPECT_EQ(profile->parts.at(profile->sources[0].first)->gameCount(), 2U);
    EXPECT_EQ(profile->parts.at(profile->sources[1].first)->gameCount(), 2U);

    // The same game from ChessBase (names written its way): your files keep it, and all of
    // them still count it once.
    const QString pasted = folder.filePath(QStringLiteral("chessbase.pgn"));
    {
        QFile file(pasted);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[White \"Carlsen,M\"]\n[Black \"Caruana,F\"]\n[Date \"2026.09.29\"]\n"
                   "[Result \"1-0\"]\n\n1.e4 e5 2.Nf3 1-0\n\n"
                   "[White \"Carlsen,M\"]\n[Black \"Anand,V\"]\n[Date \"2010.01.01\"]\n"
                   "[Result \"1/2-1/2\"]\n\n1.c4 c5 1/2-1/2\n");
    }
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    store.addOtbGames(id, {pasted});
    ASSERT_TRUE(added.wait(5000));
    EXPECT_EQ(added.front().at(1).toInt(), 2);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all->gameCount() == 4; }, 10000));
    // All four are his, also the ones ChessBase writes "Carlsen,M".
    ASSERT_TRUE(QTest::qWaitFor([&] {
        const auto& records = store.profile(id)->records;
        const auto all = records.find(QString());
        return all != records.end() && all->second.games() == 4;
    }));

    // Your PGN games and Caissabase's together: the shared game once (three, not four).
    const QString both =
        ProfileStore::partsKey({QStringLiteral("otb"), store.profile(id)->sources[0].first});
    EXPECT_EQ(store.database(id, both), nullptr); // not combined yet
    store.combine(id, both);
    ASSERT_TRUE(QTest::qWaitFor([&] { return store.database(id, both) != nullptr; }, 10000));
    EXPECT_EQ(store.database(id, both)->gameCount(), 3U);

    // A database's game deleted stays deleted when the games are looked for again.
    const auto nakamura = broadcasts->game(2);
    ASSERT_TRUE(nakamura.has_value());
    const QString broadcastPart = store.profile(id)->sources[1].first;
    store.deleteGame(id, broadcastPart, *nakamura);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all->gameCount() == 3; }, 10000));
    store.refreshSourceGames(id);
    QTest::qWait(300);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !store.busy(id); }, 10000));
    EXPECT_EQ(store.profile(id)->all->gameCount(), 3U);

    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        const kiebitz::app::AppStyle style(*qApp);
        kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
        kiebitz::app::Library library;
        kiebitz::app::RepertoireStore repertoires(library);
        kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
        view.resize(1180, 720);
        view.show();
        view.select(id);
        QTest::qWait(100);
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-sources.png")));
        const auto* parts =
            view.findChild<kiebitz::app::MultiChoice*>(QStringLiteral("profilePart"));
        QStringList texts;
        for (const QAction* action : parts->menu()->actions()) {
            texts << action->text();
        }
        EXPECT_TRUE(texts.contains(QStringLiteral("Lichess broadcasts")));
        EXPECT_TRUE(texts.contains(QStringLiteral("Caissabase")));
    }
    // Not this player after all: the databases' games go; your files stay.
    store.chooseFide(id, 0);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all->gameCount() == 2; }, 10000));
    EXPECT_TRUE(store.profile(id)->sources.empty());
}

TEST_F(ProfilesTest, GamesCopiedFromChessBaseArePastedIntoTheOverTheBoardGames) {
    const kiebitz::app::AppStyle style(*qApp);
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 720);
    view.show();
    const QString me = store.profiles()[0].id;
    view.select(me);
    QApplication::processEvents();
    QApplication::clipboard()->setText(
        QStringLiteral("[White \"Ott,L\"]\n[Black \"Smith,A\"]\n[Date \"2025.03.01\"]\n"
                       "[Result \"1-0\"]\n\n1.e4 e5 2.Nf3 1-0\n\n"
                       "[White \"Lee,B\"]\n[Black \"Ott,L\"]\n[Date \"2025.03.02\"]\n"
                       "[Result \"0-1\"]\n\n1.d4 d5 0-1\n"));
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    auto* paste = view.findChild<QPushButton*>(QStringLiteral("profilePasteGames"));
    ASSERT_NE(paste, nullptr);
    QTest::mouseClick(paste, Qt::LeftButton);
    ASSERT_TRUE(added.wait(5000));
    EXPECT_EQ(added.front().at(1).toInt(), 2);
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return !store.busy(me) && store.profile(me)->all &&
                   store.profile(me)->all->gameCount() == 2;
        },
        10000));
}

TEST(AccountFinder, UsernamesAreGuessedFromTheFideName) {
    const QStringList guesses = kiebitz::app::usernameGuesses(QStringLiteral("Wang, Justin Zhide"));
    EXPECT_EQ(guesses.front(), QStringLiteral("justinwang"));
    EXPECT_TRUE(guesses.contains(QStringLiteral("wangjustin")));
    EXPECT_TRUE(guesses.contains(QStringLiteral("jwang")));
    EXPECT_TRUE(guesses.contains(QStringLiteral("wang")));
    // Accents go: usernames are plain letters.
    EXPECT_TRUE(kiebitz::app::usernameGuesses(QStringLiteral("Müller, Jörg"))
                    .contains(QStringLiteral("jorgmuller")));
}

TEST(AccountFinder, AnAccountIsJudgedByWhatItsProfileSays) {
    kiebitz::app::FidePlayer player;
    player.name = QStringLiteral("Wang, Justin Zhide");
    player.title = QStringLiteral("CM");
    player.standard = 2103;
    using kiebitz::app::AccountFacts;
    using kiebitz::app::judgeAccount;
    // Real name and FIDE rating: surely them.
    const auto sure = judgeAccount(AccountFacts{AccountSite::Lichess,
                                                QStringLiteral("jw_chess"),
                                                QStringLiteral("Justin Wang"),
                                                {},
                                                2110},
                                   player);
    EXPECT_GE(sure.score, 90);
    EXPECT_EQ(sure.reasons.size(), 2);
    // Only the name in the username: not enough to suggest.
    EXPECT_LT(
        judgeAccount(AccountFacts{AccountSite::ChessCom, QStringLiteral("wang1999"), {}, {}, 0},
                     player)
            .score,
        50);
    // Someone else's FIDE rating counts against.
    EXPECT_LT(judgeAccount(AccountFacts{AccountSite::Lichess,
                                        QStringLiteral("justinwang"),
                                        QStringLiteral("Justin Wang"),
                                        {},
                                        1500},
                           player)
                  .score,
              50);
}

TEST(Wikidata, TheAnswerGivesTheItemItsLichessAccountsPhotoAndArticles) {
    // One row per article; the photo's file name comes percent-encoded in its address.
    const QByteArray answer =
        R"({"results":{"bindings":[)"
        R"({"item":{"value":"http://www.wikidata.org/entity/Q106807"},)"
        R"("lichess":{"value":"DrNykterstein"},)"
        R"("photo":{"value":"http://commons.wikimedia.org/wiki/Special:FilePath/Magnus%20C.jpg"},)"
        R"("article":{"value":"https://en.wikipedia.org/wiki/Magnus_Carlsen"}},)"
        R"({"item":{"value":"http://www.wikidata.org/entity/Q106807"},)"
        R"("lichess":{"value":"DrNykterstein"},)"
        R"("article":{"value":"https://no.wikipedia.org/wiki/Magnus_Carlsen"}}]}})";
    const kiebitz::app::WikiPerson person = kiebitz::app::parseWikidata(answer);
    EXPECT_EQ(person.item, QStringLiteral("Q106807"));
    EXPECT_EQ(person.lichess, QStringList{QStringLiteral("DrNykterstein")});
    EXPECT_EQ(person.photo, QStringLiteral("Magnus C.jpg"));
    // Bokmål reads the Norwegian Wikipedia; German has none here: English.
    EXPECT_EQ(person.article(QStringLiteral("nb_NO")),
              QStringLiteral("https://no.wikipedia.org/wiki/Magnus_Carlsen"));
    EXPECT_EQ(person.article(QStringLiteral("de_DE")),
              QStringLiteral("https://en.wikipedia.org/wiki/Magnus_Carlsen"));
    EXPECT_EQ(kiebitz::app::commonsPhotoPath(person.photo, 160),
              QStringLiteral("/wiki/Special:FilePath/Magnus%20C.jpg?width=160"));
    EXPECT_TRUE(kiebitz::app::wikidataQuery(1503014).contains(QStringLiteral("\"1503014\"")));

    // Two items with one FIDE id: which is meant is unclear, so neither.
    const QByteArray two = R"({"results":{"bindings":[)"
                           R"({"item":{"value":"http://www.wikidata.org/entity/Q1"}},)"
                           R"({"item":{"value":"http://www.wikidata.org/entity/Q2"}}]}})";
    EXPECT_TRUE(kiebitz::app::parseWikidata(two).item.isEmpty());
    EXPECT_TRUE(kiebitz::app::parseWikidata("not json").item.isEmpty());
}

TEST_F(ProfilesTest, WikidataGivesAPlayersLichessAccountPhotoAndWikipediaArticle) {
    server.pages["/api/fide/player"] =
        R"([{"id":4312830,"name":"Wang, Justin Zhide","federation":"NZL","title":"CM",)"
        R"("standard":2103}])";
    // Wikidata knows them: a Lichess account no username guess finds, a photo, an article.
    server.pages["/sparql"] =
        R"({"results":{"bindings":[)"
        R"({"item":{"value":"http://www.wikidata.org/entity/Q42"},)"
        R"("lichess":{"value":"KiwiKnight"},)"
        R"("photo":{"value":"http://commons.wikimedia.org/wiki/Special:FilePath/Justin%20Wang.jpg"},)"
        R"("article":{"value":"https://en.wikipedia.org/wiki/Justin_Wang"}}]}})";
    // Its profile says nothing about who it is: Wikidata alone makes it a suggestion.
    server.pages["/api/users"] = R"([{"id":"kiwiknight","username":"KiwiKnight","profile":{}}])";
    {
        QImage image(40, 60, QImage::Format_RGB32);
        image.fill(Qt::darkGreen);
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        server.pages["/wiki/Special:FilePath/Justin%20Wang.jpg"] = buffer.data();
    }
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Justin Wang"));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const auto* profile = store.profile(id);
            return profile->suggestionsFor == 4312830 && !profile->suggesting &&
                   !store.photoFile(id).isEmpty();
        },
        10000));
    const auto* profile = store.profile(id);
    ASSERT_FALSE(profile->suggestions.empty());
    EXPECT_EQ(profile->suggestions.front().username, QStringLiteral("KiwiKnight"));
    EXPECT_TRUE(
        profile->suggestions.front().reasons.contains(QStringLiteral("Listed on Wikidata")));
    EXPECT_EQ(profile->wiki.item, QStringLiteral("Q42"));
    EXPECT_FALSE(QImage(store.photoFile(id)).isNull());

    // The page: the photo by the name, the article in the FIDE card.
    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 720);
    view.show();
    view.select(id);
    auto* photo = view.findChild<QLabel*>(QStringLiteral("profilePhoto"));
    ASSERT_NE(photo, nullptr);
    EXPECT_TRUE(photo->isVisible());
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileFide")), Qt::LeftButton);
    QPushButton* article = nullptr;
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return (article = view.findChild<QPushButton*>(QStringLiteral("fideWikipedia"))); },
        5000));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-photo.png")));
    }
    if (auto* card = view.findChild<QFrame*>(QStringLiteral("fideCard"))) {
        card->close();
    }

    // Kept across starts.
    ProfileStore again(folder.filePath(QStringLiteral("profiles")));
    EXPECT_EQ(again.profile(id)->wiki, profile->wiki);
    EXPECT_FALSE(again.photoFile(id).isEmpty());

    // Another FIDE player chosen: what Wikidata said about the old one goes.
    store.chooseFide(id, 0);
    EXPECT_TRUE(store.profile(id)->wiki.item.isEmpty());
    EXPECT_TRUE(store.photoFile(id).isEmpty());
}

TEST_F(ProfilesTest, AFidePlayersAccountsAreSuggestedAndNotThemIsRemembered) {
    server.pages["/api/fide/player"] =
        R"([{"id":4312830,"name":"Wang, Justin Zhide","federation":"NZL","title":"CM",)"
        R"("standard":2103}])";
    // Lichess: the username search finds two; one's profile says who they are.
    server.pages["/api/player/autocomplete"] = R"(["JustinWang","justinwangfan"])";
    server.pages["/api/users"] =
        R"([{"id":"justinwang","username":"JustinWang","profile":{"realName":"Justin Wang",)"
        R"("fideRating":2105}},{"id":"justinwangfan","username":"justinwangfan","profile":{}}])";
    // chess.com: "jwang" exists, a verified CM with the name; everything else doesn't.
    server.pages["/pub/player/jwang"] = R"({"username":"JWang","name":"Justin Wang","title":"CM"})";
    server.pages["/pub/player/jwang/stats"] = R"({"fide":2103})";
    server.pages["/pub/titled/CM"] = R"({"players":["jwang","someoneelse"]})";
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Justin Wang"));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const auto* profile = store.profile(id);
            return profile->suggestionsFor == 4312830 && !profile->suggesting;
        },
        10000));
    const auto& suggestions = store.profile(id)->suggestions;
    ASSERT_EQ(suggestions.size(), 2U);
    QStringList names;
    for (const auto& suggestion : suggestions) {
        names << suggestion.username;
    }
    EXPECT_TRUE(names.contains(QStringLiteral("JustinWang")));
    EXPECT_TRUE(names.contains(QStringLiteral("JWang")));
    EXPECT_FALSE(names.contains(QStringLiteral("justinwangfan")));
    EXPECT_EQ(server.asked.count(QStringLiteral("/pub/player/jwang")), 1); // asked once

    // Not them: gone, and not suggested again when looked for once more.
    store.dismissSuggestion(id, AccountSite::ChessCom, QStringLiteral("JWang"));
    ASSERT_EQ(store.profile(id)->suggestions.size(), 1U);
    store.findAccounts(id, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !store.profile(id)->suggesting; }, 10000));
    QTest::qWait(50);
    ASSERT_EQ(store.profile(id)->suggestions.size(), 1U);
    EXPECT_EQ(store.profile(id)->suggestions.front().username, QStringLiteral("JustinWang"));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        const kiebitz::app::AppStyle style(*qApp);
        kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
        kiebitz::app::Library library;
        kiebitz::app::RepertoireStore repertoires(library);
        kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
        view.resize(1180, 720);
        view.show();
        view.select(id);
        QTest::qWait(100);
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-suggestions.png")));
    }
    // Kept across starts.
    ProfileStore again(folder.filePath(QStringLiteral("profiles")));
    EXPECT_EQ(again.profile(id)->suggestions.size(), 1U);
    EXPECT_TRUE(again.profile(id)->dismissed.contains(QStringLiteral("chesscom:jwang")));
}

TEST_F(ProfilesTest, PrepareKeepsTheirGamesSinceAYearAndAtSomeSpeeds) {
    // Petra's four games with Black: speeds from the events' names (none: classical).
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Petra Pawn"));
    const QString pgn = folder.filePath(QStringLiteral("petra.pgn"));
    {
        QFile file(pgn);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const auto one = [&](const char* event, const char* date, const char* result,
                             const char* moves) {
            file.write(QByteArray("[Event \"") + event +
                       "\"]\n[White \"X\"]\n[Black \"Pawn, "
                       "Petra\"]\n[Date \"" +
                       date + "\"]\n[Result \"" + result + "\"]\n\n" + moves + " " + result +
                       "\n\n");
        };
        one("Club Championship", "2026.03.01", "0-1", "1. e4 c5 2. Nf3 d6");
        one("Oslo Blitz", "2025.02.01", "1-0", "1. e4 c5 2. Nf3 Nc6");
        one("Rapid Open", "2024.04.01", "1-0", "1. e4 e5");
        one("Rapid Open", "2026.04.01", "1/2-1/2", "1. e4 c5 2. c3");
    }
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    store.addOtbGames(id, {pgn});
    ASSERT_TRUE(added.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all != nullptr; }, 10000));

    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::PreparePage page(store, repertoires, style);
    page.resize(1000, 600);
    page.QWidget::show();
    QSignalSpy ready(&page, &kiebitz::app::PreparePage::ready);
    page.show(id, QString());
    ASSERT_TRUE(ready.wait(10000));
    auto* moves = page.findChild<QTreeWidget*>(QStringLiteral("prepareMoves"));
    auto* count = page.findChild<QLabel*>(QStringLiteral("prepareCount"));
    auto* since = page.findChild<QComboBox*>(QStringLiteral("prepareSince"));
    ASSERT_NE(moves, nullptr);
    ASSERT_NE(count, nullptr);
    ASSERT_NE(since, nullptr);
    const auto games = [&](const QString& move) {
        for (int i = 0; i < moves->topLevelItemCount(); ++i) {
            if (moves->topLevelItem(i)->text(0) == move) {
                return moves->topLevelItem(i)->text(2).section(QStringLiteral(" "), 0, 0).toInt();
            }
        }
        return 0;
    };
    EXPECT_EQ(games(QStringLiteral("1. e4")), 4);
    EXPECT_TRUE(count->text().isEmpty()); // all of them
    // The years they played, newest first.
    ASSERT_EQ(since->count(), 4);
    EXPECT_EQ(since->itemData(1).toInt(), 2026);
    EXPECT_EQ(since->itemData(3).toInt(), 2024);

    // Where they struggle: after 1. e4 they score 38% (a win, a draw, two losses), and their
    // games part there (1... c5, 1... e5). 1... c5 alone is 50%: not a weak line.
    auto* weak = page.findChild<QWidget*>(QStringLiteral("prepareWeak"));
    ASSERT_NE(weak, nullptr);
    EXPECT_TRUE(weak->isVisible());
    auto* weakScore = page.findChild<QLabel*>(QStringLiteral("prepareWeakScore:0"));
    ASSERT_NE(weakScore, nullptr);
    EXPECT_EQ(weakScore->text(), QStringLiteral("38% · 4 games"));
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("prepareWeakScore:1")), nullptr);
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QTest::qWait(100); // new rows show on the next turn of the event loop
        page.grab().save(QDir(dir).filePath(QStringLiteral("prepare-weak.png")));
    }
    QTest::mouseClick(page.findChild<QPushButton*>(QStringLiteral("prepareWeak:0")),
                      Qt::LeftButton);
    EXPECT_EQ(page.line().size(), 1U);
    page.stepTo({});

    // The box opens with a click, before anything is chosen.
    QTest::mouseClick(since, Qt::LeftButton);
    EXPECT_TRUE(since->view()->isVisible());
    since->hidePopup();
    // Since 2025: three games.
    since->setCurrentIndex(2);
    emit since->activated(2);
    EXPECT_EQ(games(QStringLiteral("1. e4")), 3);
    EXPECT_EQ(count->text(), QStringLiteral("3 of 4 games"));
    // ... and only blitz (the profile page's speed filter): one.
    using kiebitz::gamedb::Speed;
    const auto bit = [](Speed speed) {
        return 1U << static_cast<unsigned>(speed);
    };
    page.setSpeeds(static_cast<std::uint8_t>(bit(Speed::Blitz)));
    EXPECT_EQ(games(QStringLiteral("1. e4")), 1);
    EXPECT_EQ(count->text(), QStringLiteral("1 of 4 games"));
    // Classical, with the games without a speed (the club championship): one.
    page.setSpeeds(static_cast<std::uint8_t>(bit(Speed::Classical) | bit(Speed::Correspondence) |
                                             bit(Speed::Unknown)));
    EXPECT_EQ(games(QStringLiteral("1. e4")), 1);
    // Every speed: since 2025, three.
    page.setSpeeds(0);
    EXPECT_EQ(games(QStringLiteral("1. e4")), 3);
    // Nothing left out: all four again.
    page.setFilter(0, 0);
    EXPECT_EQ(games(QStringLiteral("1. e4")), 4);
    EXPECT_EQ(since->currentData().toInt(), 0);
}

TEST(RatingChart, YearLabelsNeverOverlap) {
    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::RatingChart chart(style);
    chart.resize(400, 160);
    // The line starts in December 2022: "2022" would sit on top of "2023", so it gives way.
    chart.setRatings({{202212, 1560}, {202301, 1570}, {202406, 1600}, {202503, 1640}});
    auto labels = chart.yearLabels();
    ASSERT_EQ(labels.size(), 3U);
    EXPECT_EQ(labels[0].second, 2023);
    EXPECT_EQ(labels[2].second, 2025);
    // Started in January: its year is labelled.
    chart.setRatings({{202201, 1500}, {202301, 1570}, {202503, 1640}});
    labels = chart.yearLabels();
    ASSERT_EQ(labels.size(), 4U);
    EXPECT_EQ(labels[0].second, 2022);
    // A narrow chart over many years: fewer labels, none overlapping.
    chart.resize(120, 160);
    chart.setRatings({{201001, 1500}, {202512, 1900}});
    labels = chart.yearLabels();
    for (std::size_t i = 1; i < labels.size(); ++i) {
        EXPECT_GE(labels[i].first - labels[i - 1].first, 20.0);
    }
}

TEST_F(ProfilesTest, TheOverviewShowsRatingPerformanceOpponentsEventsAndAgainstYou) {
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    store.renameProfile(store.profiles()[0].id, QStringLiteral("Lukas Ott"));
    const QString id = store.addProfile(QStringLiteral("Petra Pawn"));
    const QString pgn = folder.filePath(QStringLiteral("petra.pgn"));
    {
        QFile file(pgn);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const auto one = [&](const char* event, const char* white, const char* whiteElo,
                             const char* black, const char* blackElo, const char* date,
                             const char* result) {
            file.write(QByteArray("[Event \"") + event + "\"]\n[White \"" + white +
                       "\"]\n[Black \"" + black + "\"]\n[WhiteElo \"" + whiteElo +
                       "\"]\n[BlackElo \"" + blackElo + "\"]\n[Date \"" + date + "\"]\n[Result \"" +
                       result + "\"]\n\n1. e4 e5 " + result + "\n\n");
        };
        one("Oslo Open 2025", "Pawn, Petra", "2100", "Olsen, Per", "2000", "2025.03.10", "1-0");
        one("Oslo Open 2025", "Olsen, Per", "2000", "Pawn, Petra", "2110", "2025.03.11", "0-1");
        one("Norwegian Ch 2026", "Haug, Lars", "2200", "Pawn, Petra", "2120", "2026.01.05",
            "1/2-1/2");
        one("Norwegian Ch 2026", "Pawn, Petra", "2130", "Ott, Lukas", "2050", "2026.01.06", "0-1");
    }
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    store.addOtbGames(id, {pgn});
    ASSERT_TRUE(added.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all != nullptr; }, 10000));

    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 820);
    view.show();
    view.select(id);
    auto* performance = view.findChild<QLabel*>(QStringLiteral("profilePerformance"));
    auto* opponents = view.findChild<QLabel*>(QStringLiteral("profileOpponents"));
    auto* events = view.findChild<QLabel*>(QStringLiteral("profileEvents"));
    auto* vsYou = view.findChild<QLabel*>(QStringLiteral("profileAgainstYou"));
    ASSERT_NE(performance, nullptr);
    ASSERT_NE(opponents, nullptr);
    ASSERT_NE(events, nullptr);
    ASSERT_NE(vsYou, nullptr);
    // Opponents 2000, 2000, 2200, 2050 (2063); 2½ of 4 = 63% → +89.
    ASSERT_TRUE(QTest::qWaitFor([&] { return !performance->text().isEmpty(); }, 10000));
    EXPECT_EQ(performance->text(),
              QStringLiteral("Performance 2152  ·  opponents 2063  ·  4 games"));
    EXPECT_TRUE(opponents->text().contains(QStringLiteral("Olsen, Per")));
    EXPECT_TRUE(opponents->text().contains(QStringLiteral("2 · 100%")));
    // The latest tournament first, with her points.
    EXPECT_LT(events->text().indexOf(QStringLiteral("Norwegian Ch 2026")),
              events->text().indexOf(QStringLiteral("Oslo Open 2025")));
    EXPECT_TRUE(events->text().contains(QStringLiteral("½/2")));
    EXPECT_TRUE(events->text().contains(QStringLiteral("2/2")));
    // Her rating: FIDE classical (her games have no time control: classical over the board),
    // the last of each month. No other speed has ratings.
    auto* chart = view.findChild<kiebitz::app::RatingChart*>(QStringLiteral("ratingChart"));
    auto* from = view.findChild<QComboBox*>(QStringLiteral("ratingFrom"));
    ASSERT_NE(chart, nullptr);
    ASSERT_NE(from, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return chart->ratings().size() == 2; }, 10000));
    EXPECT_EQ(chart->ratings()[0], std::make_pair(202503, 2110));
    EXPECT_EQ(chart->ratings()[1], std::make_pair(202601, 2130));
    auto* ratingSpeed = view.findChild<QLabel*>(QStringLiteral("ratingSpeed"));
    ASSERT_NE(ratingSpeed, nullptr);
    EXPECT_EQ(from->count(), 1);
    EXPECT_EQ(from->currentText(), QStringLiteral("FIDE"));
    EXPECT_EQ(ratingSpeed->text(), QStringLiteral("Classical"));
    EXPECT_TRUE(view.findChild<QPushButton*>(QStringLiteral("profileSpeed:all"))->isChecked());
    // I beat her once: +1 from my side.
    EXPECT_TRUE(vsYou->isVisible());
    EXPECT_TRUE(vsYou->text().contains(QStringLiteral("+1 =0 −0")));
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        QTest::qWait(100);
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-overview.png")));
    }
    // The page's speed filter: blitz has no games, nor ratings; the games list is empty too.
    auto* table = view.findChild<QTableView*>(QStringLiteral("profileGames"));
    ASSERT_NE(table, nullptr);
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileSpeed:blitz")),
                      Qt::LeftButton);
    ASSERT_TRUE(QTest::qWaitFor([&] { return performance->text().isEmpty(); }, 10000));
    EXPECT_EQ(ratingSpeed->text(), QStringLiteral("Blitz"));
    EXPECT_TRUE(chart->ratings().empty());
    EXPECT_FALSE(from->isVisibleTo(&view));
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 0; }, 10000));
    auto* all = view.findChild<QPushButton*>(QStringLiteral("profileSpeed:all"));
    auto* blitz = view.findChild<QPushButton*>(QStringLiteral("profileSpeed:blitz"));
    auto* classical = view.findChild<QPushButton*>(QStringLiteral("profileSpeed:classical"));
    EXPECT_FALSE(all->isChecked());
    // Classical too: her games without a time control are; the line is the classical one.
    QTest::mouseClick(classical, Qt::LeftButton);
    EXPECT_TRUE(blitz->isChecked());
    ASSERT_TRUE(QTest::qWaitFor([&] { return !performance->text().isEmpty(); }, 10000));
    EXPECT_EQ(ratingSpeed->text(), QStringLiteral("Classical"));
    EXPECT_EQ(chart->ratings().size(), 2U);
    EXPECT_TRUE(from->isVisibleTo(&view));
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->model()->rowCount() == 4; }, 10000));
    // Both off again: "All".
    QTest::mouseClick(blitz, Qt::LeftButton);
    QTest::mouseClick(classical, Qt::LeftButton);
    EXPECT_TRUE(all->isChecked());
    // "All" turns the speeds off.
    QTest::mouseClick(blitz, Qt::LeftButton);
    QTest::mouseClick(all, Qt::LeftButton);
    EXPECT_TRUE(all->isChecked());
    EXPECT_FALSE(blitz->isChecked());
    // The rating source is a filter at the top, on the Overview tab only.
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileShowGames")),
                      Qt::LeftButton);
    EXPECT_FALSE(from->isVisibleTo(&view));
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("profileShowOverview")),
                      Qt::LeftButton);
    EXPECT_TRUE(from->isVisibleTo(&view));
    // My own profile: nothing against myself.
    view.select(store.profiles()[0].id);
    EXPECT_FALSE(vsYou->isVisible());
}

TEST_F(ProfilesTest, PreparingShowsTheirLinesWhatYourRepertoireLacksAndWhereTheyLeaveTheory) {
    // Your White repertoire: the Open Sicilian with 2... d6.
    const QTemporaryDir libraryFolder;
    {
        QFile file(libraryFolder.filePath(QStringLiteral("white.pgn")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[FileType \"Repertoire\"]\n[Orientation \"white\"]\n\n"
                   "1. e4 c5 2. Nf3 d6 3. d4 *\n");
    }
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    library.setFolder(libraryFolder.path());
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return repertoires.get(kiebitz::core::Color::White) != nullptr; }, 10000));

    // The opponent: 2... Nc6 twice and 2... a6 once as Black; 1. c4 twice as White.
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString id = store.addProfile(QStringLiteral("Petra Pawn"));
    const QString pgn = folder.filePath(QStringLiteral("petra.pgn"));
    {
        QFile file(pgn);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const auto one = [&](const char* white, const char* black, const char* date,
                             const char* result, const char* moves) {
            file.write(QByteArray("[White \"") + white + "\"]\n[Black \"" + black +
                       "\"]\n[Date \"" + date + "\"]\n[Result \"" + result + "\"]\n\n" + moves +
                       " " + result + "\n\n");
        };
        one("A", "Pawn, Petra", "2026.09.01", "0-1", "1. e4 c5 2. Nf3 Nc6 3. d4 cxd4");
        one("B", "Pawn, Petra", "2025.01.01", "1/2-1/2", "1. e4 c5 2. Nf3 Nc6 3. Bb5 g6");
        one("C", "Pawn, Petra", "2024.01.01", "1-0", "1. e4 c5 2. Nf3 a6");
        one("Pawn, Petra", "D", "2026.05.01", "1-0", "1. c4 e5 2. Nc3");
        one("Pawn, Petra", "E", "2026.06.01", "1-0", "1. c4 e5 2. g3");
    }
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    store.addOtbGames(id, {pgn});
    ASSERT_TRUE(added.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(id) && store.profile(id)->all != nullptr; }, 10000));

    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::ProfilesView view(store, lichess, repertoires, style);
    view.resize(1180, 720);
    view.show();
    view.select(id);
    auto* prepareButton = view.findChild<QPushButton*>(QStringLiteral("profileShowPrepare"));
    ASSERT_NE(prepareButton, nullptr);
    EXPECT_TRUE(prepareButton->isVisible());
    auto* page = view.findChild<kiebitz::app::PreparePage*>();
    QSignalSpy ready(page, &kiebitz::app::PreparePage::ready);
    QTest::mouseClick(prepareButton, Qt::LeftButton);
    ASSERT_TRUE(ready.wait(10000));

    auto* moves = view.findChild<QTreeWidget*>(QStringLiteral("prepareMoves"));
    ASSERT_NE(moves, nullptr);
    const auto row = [&](const QString& move) -> QTreeWidgetItem* {
        for (int i = 0; i < moves->topLevelItemCount(); ++i) {
            if (moves->topLevelItem(i)->text(0) == move) {
                return moves->topLevelItem(i);
            }
        }
        return nullptr;
    };
    const auto step = [&](const QString& move) {
        QTreeWidgetItem* item = row(move);
        ASSERT_NE(item, nullptr) << move.toStdString();
        emit moves->itemClicked(item, 0);
    };
    // I play White: from the start, my 1. e4 (her 3 games with Black), my repertoire's.
    ASSERT_NE(row(QStringLiteral("1. e4")), nullptr);
    EXPECT_EQ(row(QStringLiteral("1. e4"))->text(2), QStringLiteral("3 · 100%"));
    // A click steps in: her replies, then mine, then hers again.
    step(QStringLiteral("1. e4"));
    EXPECT_EQ(row(QStringLiteral("1… c5"))->text(2), QStringLiteral("3 · 100%"));
    step(QStringLiteral("1… c5"));
    step(QStringLiteral("2. Nf3"));
    EXPECT_EQ(page->line().size(), 3U);
    ASSERT_NE(row(QStringLiteral("2… Nc6")), nullptr);
    EXPECT_EQ(row(QStringLiteral("2… Nc6"))->text(2), QStringLiteral("2 · 67%"));
    EXPECT_EQ(row(QStringLiteral("2… a6"))->text(2), QStringLiteral("1 · 33%"));
    // The board shows where I am.
    const auto* board = view.findChild<kiebitz::app::BoardView*>(QStringLiteral("prepareBoard"));
    ASSERT_NE(board, nullptr);
    EXPECT_EQ(board->position().fullmoveNumber(), 2);

    // The path leads back.
    auto* back = view.findChild<QPushButton*>(QStringLiteral("preparePath:1"));
    ASSERT_NE(back, nullptr);
    QTest::mouseClick(back, Qt::LeftButton);
    EXPECT_EQ(page->line().size(), 1U);
    // The moves ahead stay: the path still has them, and the arrows step along them.
    ASSERT_NE(view.findChild<QPushButton*>(QStringLiteral("preparePath:3")), nullptr);
    auto* forward = view.findChild<QPushButton*>(QStringLiteral("prepareForward"));
    auto* stepBack = view.findChild<QPushButton*>(QStringLiteral("prepareBack"));
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(stepBack, nullptr);
    QTest::mouseClick(forward, Qt::LeftButton);
    EXPECT_EQ(page->line().size(), 2U);
    QTest::keyClick(page, Qt::Key_Right);
    EXPECT_EQ(page->line().size(), 3U);
    EXPECT_FALSE(forward->isEnabled()); // the end of the line
    QTest::mouseClick(stepBack, Qt::LeftButton);
    QTest::keyClick(page, Qt::Key_Left);
    EXPECT_EQ(page->line().size(), 1U);
    QTest::keyClick(page, Qt::Key_Home);
    EXPECT_TRUE(page->line().empty());
    EXPECT_FALSE(stepBack->isEnabled());
    QTest::keyClick(page, Qt::Key_End);
    EXPECT_EQ(page->line().size(), 3U);
    // Another move from the middle: the old moves ahead are gone.
    QTest::keyClick(page, Qt::Key_Left);
    step(QStringLiteral("2. Nf3")); // the same move: still on the way
    EXPECT_EQ(page->line().size(), 3U);
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("preparePath:1")),
                      Qt::LeftButton);
    step(QStringLiteral("1… c5"));
    EXPECT_EQ(page->line().size(), 2U);
    step(QStringLiteral("2. Nf3"));

    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        view.findChild<QPushButton*>(QStringLiteral("prepareAdd:2… Nc6"))->click();
        QTest::qWait(100); // new rows and chips show on the next turn of the event loop
        view.grab().save(QDir(dir).filePath(QStringLiteral("profile-prepare.png")));
        view.findChild<QPushButton*>(QStringLiteral("prepareAdd:2… Nc6"))->click();
    }
    // + puts the line into my prep; the file has it, with her numbers as comments.
    auto* add = view.findChild<QPushButton*>(QStringLiteral("prepareAdd:2… Nc6"));
    ASSERT_NE(add, nullptr);
    add->click();
    ASSERT_EQ(page->prepLines().size(), 1U);
    auto* create = view.findChild<QPushButton*>(QStringLiteral("prepareCreate"));
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(create->isVisible());
    QSignalSpy file(&view, &kiebitz::app::ProfilesView::prepFileRequested);
    QTest::mouseClick(create, Qt::LeftButton);
    ASSERT_EQ(file.size(), 1);
    EXPECT_EQ(file.front().at(0).toString(), QStringLiteral("Petra Pawn – White"));
    std::string text = kiebitz::pgn::writeGame(file.front().at(1).value<kiebitz::core::Game>());
    std::replace(text.begin(), text.end(), '\n', ' ');
    EXPECT_NE(text.find("[FileType \"Repertoire\"]"), std::string::npos) << text;
    EXPECT_NE(text.find("2. Nf3 Nc6 {Petra Pawn: 2 games, scores 75%"), std::string::npos) << text;
    // Clicked again, it comes out.
    view.findChild<QPushButton*>(QStringLiteral("prepareAdd:2… Nc6"))->click();
    EXPECT_TRUE(page->prepLines().empty());

    // Your repertoire changed: where I am stays.
    page->show(id, QString(), true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return row(QStringLiteral("2… Nc6")) != nullptr; }));
    EXPECT_EQ(page->line().size(), 3U);

    // I play Black: her White games, 1. c4 twice, both won; from the start again.
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("prepareBlack")), Qt::LeftButton);
    EXPECT_TRUE(page->line().empty());
    ASSERT_NE(row(QStringLiteral("1. c4")), nullptr);
    EXPECT_EQ(row(QStringLiteral("1. c4"))->text(2), QStringLiteral("2 · 100%"));
    EXPECT_TRUE(board->isFlipped());

    // "Open on the board": the moves so far, in a new game.
    step(QStringLiteral("1. c4"));
    QSignalSpy opened(&view, &kiebitz::app::ProfilesView::openGameRequested);
    QTest::mouseClick(view.findChild<QPushButton*>(QStringLiteral("prepareOpen")), Qt::LeftButton);
    ASSERT_EQ(opened.size(), 1);
    EXPECT_EQ(opened.front().at(0).value<kiebitz::core::Game>().mainLine().size(), 2U);
}

TEST_F(ProfilesTest, TheExplorerShowsHowOftenAProfilesMovesArePlayedInTheory) {
    // A reference database: 1. e4 three times, 1. d4 once.
    kiebitz::app::DatabaseManager databases(folder.filePath(QStringLiteral("dbs")));
    const QString reference = folder.filePath(QStringLiteral("reference.pgn"));
    {
        QFile file(reference);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        for (const char* moves : {"1. e4 e5", "1. e4 c5", "1. e4 e6", "1. d4 d5"}) {
            file.write(QByteArray("[White \"X\"]\n[Black \"Y\"]\n[Result \"*\"]\n\n") + moves +
                       " *\n\n");
        }
    }
    QSignalSpy imported(&databases, &kiebitz::app::DatabaseManager::jobFinished);
    databases.importFile(reference, QStringLiteral("Reference"));
    ASSERT_TRUE(imported.wait(10000));
    ASSERT_TRUE(QTest::qWaitFor([&] { return databases.databases().size() == 1; }, 5000));

    // A profile who plays 1. d4 and 1. b3.
    ProfileStore store(folder.filePath(QStringLiteral("profiles")));
    store.setServers(server.url(), server.url(), server.url());
    const QString me = store.profiles()[0].id;
    const QString pgn = folder.filePath(QStringLiteral("mine.pgn"));
    {
        QFile file(pgn);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("[White \"Me\"]\n[Black \"A\"]\n[Result \"*\"]\n\n1. d4 d5 *\n\n"
                   "[White \"Me\"]\n[Black \"B\"]\n[Result \"*\"]\n\n1. b3 e5 *\n");
    }
    QSignalSpy added(&store, &ProfileStore::otbAdded);
    store.addOtbGames(me, {pgn});
    ASSERT_TRUE(added.wait(5000));
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !store.busy(me) && store.profile(me)->all != nullptr; }, 10000));

    const kiebitz::app::AppStyle style(*qApp);
    kiebitz::app::LichessLogin lichess{kiebitz::app::LichessLogin::Storage{}};
    kiebitz::app::Library library;
    kiebitz::app::RepertoireStore repertoires(library);
    kiebitz::app::ExplorerPanel explorer(databases, store, repertoires, lichess, style);
    auto* source = explorer.findChild<QComboBox*>(QStringLiteral("explorerSource"));
    source->setCurrentIndex(source->findData(QStringLiteral("profile:") + me));
    emit source->activated(source->currentIndex());
    const kiebitz::core::Game start;
    explorer.setPosition(&start, kiebitz::core::Game::root(), kiebitz::core::Color::White);
    // Their moves, with how often the reference plays them: d4 in a quarter, b3 never.
    ASSERT_TRUE(QTest::qWaitFor(
        [&] { return !explorer.theoryShare(QStringLiteral("d4")).isEmpty(); }, 5000));
    EXPECT_EQ(explorer.theoryShare(QStringLiteral("d4")), QStringLiteral("25%"));
    EXPECT_EQ(explorer.theoryShare(QStringLiteral("b3")), QStringLiteral("–"));

    // The reference itself as the source: no theory column.
    source->setCurrentIndex(
        source->findData(QStringLiteral("db:") + databases.databases()[0].folder));
    emit source->activated(source->currentIndex());
    ASSERT_TRUE(QTest::qWaitFor([&] { return explorer.moveTexts().size() == 2; }, 5000));
    EXPECT_EQ(explorer.theoryShare(QStringLiteral("e4")), QString());
}
