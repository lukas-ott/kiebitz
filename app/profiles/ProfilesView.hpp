#pragma once

#include "board/BoardTheme.hpp"
#include "kiebitz/core/game.hpp"
#include "kiebitz/gamedb/database.hpp"
#include "kiebitz/gamedb/facts.hpp"
#include "kiebitz/repertoire/theory.hpp"

#include <QSet>
#include <QWidget>

#include <cstdint>
#include <map>
#include <memory>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableView;

namespace kiebitz::app {

class ElidedLabel;

class ActivityChart;
class RatingChart;
class AppStyle;
class DateRange;
class GamesModel;
class LichessLogin;
class ResultBar;
class PageHeader;
class ProfileStore;
class RepertoireStore;
struct Profile;

/// The Profiles page: you and the people whose games you follow (friends, opponents). Each
/// profile has accounts (Lichess, chess.com) whose games are downloaded, and over-the-board
/// games you add as PGN; all of them together are the profile's database, which can be looked
/// at whole or one account at a time.
class ProfilesView : public QWidget {
    Q_OBJECT

public:
    ProfilesView(ProfileStore& store, LichessLogin& lichess, RepertoireStore& repertoires,
                 const AppStyle& style, QWidget* parent = nullptr);

    /// For tests: the theory check's rows ("2. Bc4  You  2  2. Nf3") and its summary.
    [[nodiscard]] QStringList theoryRows() const;
    [[nodiscard]] QString theorySummary() const;

    /// The profile shown (its id).
    [[nodiscard]] QString current() const { return m_current; }
    void select(const QString& profileId);
    /// Games copied as PGN (from ChessBase's online database), into the over-the-board games.
    void pasteGames();
    /// The Prepare tab's board: colours and pieces as on the Board page.
    void setBoardLook(const BoardTheme& theme, const QString& pieces);

signals:
    void openGameRequested(const kiebitz::core::Game& game);
    /// A game from the theory check: opened at the move that left the book (`ply`: moves from
    /// the start, that move included).
    void openGameAtRequested(const kiebitz::core::Game& game, int ply);
    /// The repertoire file for `side` with the position after `line`, opened there.
    void repertoireRequested(kiebitz::core::Color side,
                             const std::vector<kiebitz::core::Move>& line);
    /// The Prepare tab's "Create Prep File": a repertoire file to save in the Library.
    void prepFileRequested(const QString& name, const kiebitz::core::Game& game);
    /// The theory check is done (tests).
    void theoryChecked();

private:
    /// A sync's or lookup's status in the summary's place; empty: the summary again.
    void showStatus(const QString& status);
    void refreshList();
    void showProfile();
    void showAccounts(const Profile& profile);
    /// Your main profile: the Lichess login, as a card above the accounts.
    void showLichessLogin();
    void showParts(const Profile& profile);
    void showGames();
    void showStats();
    /// The Repertoire tab: where the games shown left your repertoire (in the background).
    void showTheory();
    void fillTheory();
    void newProfile();
    void addAccount();
    void addOtbGames();
    void showFideCard();
    void showPhoto();
    /// Performance, rating, opponents, events, against you: worked out in the background.
    void showFacts();
    void showFacts(const gamedb::PlayerFacts& facts);
    /// Their ratings from every source, read again when the profile's games changed.
    void showRatings();
    /// The line for the speed and source chosen (or the nearest there is).
    void showRatingLine();
    /// The speed chips ticked: the filter for every tab.
    void speedsChanged();
    /// The rating source box: on the Overview tab, when there is a rating to choose.
    void showRatingFrom();
    /// The game selected in the Games tab, deleted from the profile (after asking).
    void deleteSelectedGame();

    ProfileStore& m_store;
    RepertoireStore& m_repertoires;
    LichessLogin& m_lichess;
    QString m_signingInFor; ///< the Lichess account whose card's "Sign In" was clicked
    const AppStyle& m_style;
    QString m_current;
    PageHeader* m_header = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_name = nullptr;
    QPushButton* m_info = nullptr; ///< "i": the profile's FIDE player
    QLabel* m_photo = nullptr;     ///< the player's photo (Wikimedia Commons), if there is one
    QString m_photoShown;          ///< the photo file shown, and when it was written
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QStackedWidget* m_summaryLine = nullptr; ///< m_summary, or m_status while there is one
    QPushButton* m_sync = nullptr;
    QPushButton* m_rename = nullptr;
    QPushButton* m_remove = nullptr;
    QVBoxLayout* m_accounts = nullptr;    ///< the account cards, one above the other
    class MultiChoice* m_parts = nullptr; ///< games from: accounts, over the board, databases
    QString m_part;                       ///< the part looked at ("" = all)
    /// The time controls looked at, on every tab: "All", or some of the speeds. Classical takes
    /// in correspondence and games without a known speed (over the board, mostly classical).
    QPushButton* m_allSpeeds = nullptr;
    std::vector<std::pair<gamedb::Speed, QPushButton*>> m_speedChips;
    std::uint8_t m_speeds = 0;         ///< the same as bits: 1 << Speed; 0: all
    QStackedWidget* m_pages = nullptr; ///< Overview | Games | Repertoire | Prepare
    class PreparePage* m_prepare = nullptr;
    QPushButton* m_showRepertoire = nullptr; ///< your own profile only
    QPushButton* m_showPrepare = nullptr;    ///< someone else's only
    QLabel* m_totalText = nullptr;           ///< "1,234 games · 54%"
    ResultBar* m_totalBar = nullptr;
    QLabel* m_legend = nullptr; ///< wins, draws, losses with their numbers
    QLabel* m_whiteText = nullptr;
    ResultBar* m_whiteBar = nullptr;
    QLabel* m_blackText = nullptr;
    ResultBar* m_blackBar = nullptr;
    ActivityChart* m_activity = nullptr;
    RatingChart* m_rating = nullptr;
    /// Where the rating line comes from: FIDE (the over-the-board games), or an account.
    struct RatingSource {
        QString key; ///< "fide", or the account's id
        QString text;
        gamedb::RatingHistory history;
    };
    std::vector<RatingSource> m_ratingSources;
    class QComboBox* m_ratingFrom = nullptr; ///< in the filters, on the Overview tab
    QWidget* m_ratingFromBox = nullptr;      ///< "Rating" and m_ratingFrom
    QString m_ratingSource = QStringLiteral("fide");
    QLabel* m_ratingSpeedText = nullptr; ///< the speed of the line: "Classical"
    QString m_ratingStamp;               ///< the profile and databases the sources were read from
    int m_ratingsRequest = 0;
    QLabel* m_performance = nullptr; ///< "Performance 2148 · opponents 2067 · 34 games"
    QLabel* m_opponents = nullptr;   ///< who they played most
    QLabel* m_events = nullptr;      ///< their latest tournaments
    QWidget* m_vsYouTile = nullptr;  ///< your results against them (someone else's profile)
    QLabel* m_vsYou = nullptr;
    int m_statsRequest = 0;
    int m_factsRequest = 0;
    // The theory check
    QLabel* m_theorySummary = nullptr;
    QTreeWidget* m_theory = nullptr;
    int m_theoryKind = -1; ///< the departures shown: -1 all, else a repertoire::Departure
    int m_theoryRequest = 0;
    repertoire::TheoryReport m_theoryReport;
    std::vector<gamedb::GameInfo> m_theoryGames; ///< the games checked, as the report counts them
    std::vector<std::shared_ptr<const gamedb::Database>> m_theorySources; ///< with their games
    /// Results kept, by profile and part, with what they were worked out from (repertoire
    /// files, the games, the names). Also saved in the profile's folder, so the tab opens at
    /// once; checked again only when that changes.
    struct SavedTheory {
        QString fingerprint;
        repertoire::TheoryReport report;
        std::vector<gamedb::GameInfo> games;
    };
    std::map<QString, SavedTheory> m_theorySaved;
    [[nodiscard]] QString theoryFingerprint(const Profile& profile) const;
    // New games: your games since you last marked them seen (after a sync, to look at).
    static constexpr int kNewGames = 3; ///< m_theoryKind: only the new games
    /// A game, the same in every rebuild of the database: where, who, when and how long.
    [[nodiscard]] static QString gameKey(const gamedb::GameInfo& game);
    [[nodiscard]] bool isNew(const gamedb::GameInfo& game) const;
    /// Reads the games seen (the first time: all games are) and counts the new ones.
    void countNewGames();
    /// Every game of the profile is seen now.
    void markGamesSeen();
    QPushButton* m_markSeen = nullptr;
    QPushButton* m_newChip = nullptr;
    QString m_seenProfile; ///< whose m_seen is
    QSet<QString> m_seen;  ///< gameKey()s
    int m_newCount = 0;    ///< of the profile shown
    int m_newRequest = 0;
    QLineEdit* m_search = nullptr;
    DateRange* m_dates = nullptr;
    ElidedLabel* m_gamesStatus = nullptr;
    QTableView* m_games = nullptr;
    GamesModel* m_model = nullptr;
    std::shared_ptr<const gamedb::Database> m_db; ///< the games shown
};

} // namespace kiebitz::app
