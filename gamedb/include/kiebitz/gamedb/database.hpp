#pragma once

#include "kiebitz/core/game.hpp"
#include "kiebitz/core/position.hpp"
#include "kiebitz/core/result.hpp"
#include "kiebitz/gamedb/scan.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// A game database: millions of games in a folder, searchable, with an opening explorer.
///
/// The folder holds:
///   fide.bin      (only if its games have FIDE ids) pairs of a FIDE id and a game, sorted;
///   games.sqlite  the headers, with indexes for searching players, events and openings;
///   moves.bin     every game's main line, one byte per move;
///   offsets.bin   where each game's moves start in moves.bin;
///   order.bin     the games from the usual start position, sorted by their moves;
///   sums.bin      running totals of their results in that order;
///   elo.bin       each game's average rating;
///   ratings.bin   White's and Black's rating of each game, in the order of order.bin;
///   attrs.bin     each game's year, speed and players, in the same order (for filters);
///   positions.bin the positions of the first kPositionPlies plies that at least two games
///                 reach, each with the range of order.bin that reaches it by one move order.
/// Games with the same opening sit next to each other in order.bin, so a line's games are one
/// range found by binary search, and its statistics are two lookups in sums.bin: the explorer
/// answers in well under a millisecond for any number of games. positions.bin adds up the
/// other move orders into the same position (transpositions), and finds a position set up
/// from a FEN.
namespace kiebitz::gamedb {

using GameId = std::int64_t; ///< 1, 2, ... in the order the games were imported

struct GameInfo {
    GameId id = 0;
    std::string white, black, event, site, date, round, eco;
    int whiteElo = 0;
    int blackElo = 0;
    GameResult result = GameResult::Unknown;
    int plies = 0;
};

/// A database's description (also kept in it).
struct DatabaseInfo {
    std::string name;
    std::string source; ///< where it came from: a file path or a download's name
    std::uint64_t games = 0;
    std::uint64_t players = 0;
    /// Most of its games were played online (lichess.org, chess.com): online blitz and bullet
    /// ratings go far higher than over-the-board ones, which matters for telling engines apart.
    bool online = false;
};

/// The highest rating a human has in a database; games rated above it are engines'. Over the
/// board no one has been above 2882; online, blitz and bullet ratings reach about 3300.
[[nodiscard]] constexpr int highestHumanRating(const DatabaseInfo& info) {
    return info.online ? 3300 : 2900;
}

/// What the explorer shows for a line: its games and results, and every next move played.
struct MoveStats {
    core::Move move;
    std::string san;
    std::uint64_t games = 0;
    std::uint64_t white = 0; ///< White won
    std::uint64_t draws = 0;
    std::uint64_t black = 0; ///< Black won
    int whiteElo = 0;        ///< White's average rating in these games (0: none known)
    int blackElo = 0;
};
struct Explored {
    std::uint64_t games = 0;
    std::uint64_t white = 0;
    std::uint64_t draws = 0;
    std::uint64_t black = 0;
    int whiteElo = 0;
    int blackElo = 0;
    std::vector<MoveStats> moves; ///< most played first
};

/// A player's results, from the player's side: wins, draws, losses with each colour.
struct Record {
    std::array<std::uint64_t, 3> asWhite{}; ///< won, drawn, lost
    std::array<std::uint64_t, 3> asBlack{};
    [[nodiscard]] std::uint64_t games() const {
        return asWhite[0] + asWhite[1] + asWhite[2] + asBlack[0] + asBlack[1] + asBlack[2];
    }
    Record& operator+=(const Record& other) {
        for (std::size_t k = 0; k < 3; ++k) {
            asWhite[k] += other.asWhite[k];
            asBlack[k] += other.asBlack[k];
        }
        return *this;
    }
};

/// A date as typed ("2024", "2024-5", "2024.05.12", "12.05.2024") as the start of a PGN date
/// ("2024", "2024.05", "2024.05.12"), or nothing if it isn't one.
[[nodiscard]] std::optional<std::string> pgnDatePrefix(std::string_view typed);
/// Whether a PGN date ("2024.05.12", "2024.??.??") lies in a range of such prefixes (either may
/// be empty: no limit). A date not known that far (the year missing) is outside any limit.
[[nodiscard]] bool dateInRange(std::string_view date, std::string_view from, std::string_view to);

/// Which games a games table shows, in which order.
struct GameQuery {
    std::string text;     ///< in a player's name, the event or site (any case), or the ECO
    std::string fromDate; ///< a pgnDatePrefix(); empty: no limit
    std::string toDate;
    std::uint8_t speeds = 0; ///< only games at these speeds (bits: 1 << Speed); 0: all
    enum class Order : std::uint8_t {
        Id,
        White,
        WhiteElo,
        Black,
        BlackElo,
        Result,
        Date,
        Event,
        Eco
    };
    Order order = Order::Id; ///< Id: as stored (with text: newest first)
    bool descending = false;
    /// All games as stored: nothing to search, filter or sort.
    [[nodiscard]] bool plain() const {
        return text.empty() && fromDate.empty() && toDate.empty() && speeds == 0 &&
               order == Order::Id && !descending;
    }
};

/// Which games the explorer counts. Empty fields don't filter.
struct ExploreFilter {
    int minRating = 0; ///< the players' average rating at least this
    int maxRating = 0; ///< ... and at most this
    int fromYear = 0;
    int toYear = 0;
    std::uint8_t speeds = 0; ///< bits: 1 << Speed (Unknown included when any bit is set: never)
    /// Only the games of these players (any case)...
    std::vector<std::string> players;
    /// ... with this colour.
    enum class Side : std::uint8_t { Any, White, Black } side = Side::Any;
    /// Only games whose White (Black) player's name contains this (any case): "carlsen".
    std::string white;
    std::string black;

    [[nodiscard]] bool active() const {
        return minRating > 0 || maxRating > 0 || fromYear > 0 || toYear > 0 || speeds != 0 ||
               !players.empty() || !white.empty() || !black.empty();
    }
};

/// A player's results in total and by year (0: no date), for the profile's overview.
struct PlayerStats {
    Record total;
    std::map<int, Record> years;
};

/// Writes a new database: add() every game, then finish().
class DatabaseWriter {
public:
    /// Creates the folder's files (the folder must exist and be empty).
    [[nodiscard]] static core::Result<std::unique_ptr<DatabaseWriter>>
    create(const std::string& folder, DatabaseInfo info);
    ~DatabaseWriter();
    DatabaseWriter(const DatabaseWriter&) = delete;
    DatabaseWriter& operator=(const DatabaseWriter&) = delete;
    DatabaseWriter(DatabaseWriter&&) = delete;
    DatabaseWriter& operator=(DatabaseWriter&&) = delete;

    void add(const ScannedGame& game);
    [[nodiscard]] std::uint64_t count() const;

    /// Sorts the games and writes the indexes; `progress` gets 0..1. Empty on success, else
    /// the error.
    std::string finish(const std::function<void(double)>& progress = {});

private:
    struct Impl;
    explicit DatabaseWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

/// Adds what a database made by an older Kiebitz lacks (ratings.bin, attrs.bin) from the
/// headers it has. Empty on success, else the error. A Database opened before keeps working
/// without them; one opened afterwards has them. (Speeds aren't in the headers kept: those
/// stay unknown.)
std::string_view upgrade(const std::string& folder);

/// Adds the indexes that make sorting the games list fast (by rating, result, a player's or an
/// event's name) to a database made before them. Empty on success, else the error. Takes a
/// while on millions of games: for a background thread.
std::string_view addSortIndexes(const std::string& folder);

/// How deep positions.bin goes: positions after up to this many plies (20 moves each).
inline constexpr int kPositionPlies = 40;

/// Adds positions.bin to a database made before it (and is what finish() uses). Empty on
/// success, else the error. Goes through every game once: a minute or two for millions, for a
/// background thread. `progress` gets 0..1.
std::string_view addPositions(const std::string& folder,
                              const std::function<void(double)>& progress = {});

/// Reads a finished database. Safe to use from several threads at once.
class Database {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<Database>> open(const std::string& folder);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    [[nodiscard]] const DatabaseInfo& info() const;
    /// Whether it has ratings.bin and attrs.bin (older databases get them from upgrade()).
    [[nodiscard]] bool hasRatings() const;
    [[nodiscard]] bool hasAttributes() const;
    /// Whether it has the indexes for sorting (older databases get them from addSortIndexes()).
    [[nodiscard]] bool hasSortIndexes() const;
    /// Whether it has positions.bin (older databases get it from addPositions()).
    [[nodiscard]] bool hasPositions() const;
    [[nodiscard]] std::uint64_t gameCount() const;
    /// Whether its games have their players' FIDE ids (fide.bin).
    [[nodiscard]] bool hasFideIds() const;
    /// The games of the player with this FIDE id, as White or Black, in the order imported.
    [[nodiscard]] std::vector<GameId> gamesOfFideId(std::uint32_t fideId) const;
    /// The games of the player called any of `names` exactly (any case: "Carlsen, Magnus"), as
    /// White or Black, in the order imported. Any thread.
    [[nodiscard]] std::vector<GameId> gamesOfPlayer(std::span<const std::string> names) const;

    /// Games by id, `count` of them from `first` (for scrolling through all of them).
    [[nodiscard]] std::vector<GameInfo> games(GameId first, std::size_t count) const;
    /// Games whose players, event, site or ECO contain `text` (any case), newest first.
    [[nodiscard]] std::vector<GameInfo> search(std::string_view text, std::size_t limit) const;
    /// Reads what a search looks in (the names), if it hasn't yet: the first search is then as
    /// quick as the rest. Any thread; call it before searching (the Databases page does when a
    /// database is shown).
    void prepareSearch() const;
    /// The games a query asks for, at most `limit` of them. On a connection of its own, so a
    /// slow one (sorting millions of games) holds up nothing else; `cancelled`, when set, stops
    /// it (and it returns what it has, which is to be thrown away). The text's words are looked
    /// for in names in any order; under three letters in all, the text isn't searched yet.
    [[nodiscard]] std::vector<GameInfo> find(const GameQuery& asked, std::size_t limit,
                                             const std::atomic<bool>* cancelled = nullptr) const;
    /// A whole game: headers and moves.
    [[nodiscard]] std::optional<core::Game> game(GameId id) const;
    /// Each game's speed (from its [TimeControl] or event when it was imported), by id - 1:
    /// Unknown for games from a set-up position, or a database from before speeds were kept.
    [[nodiscard]] std::vector<Speed> speeds() const;

    /// The results of the player(s) called any of `names` (any case), in finished games.
    [[nodiscard]] Record record(std::span<const std::string> names) const;
    /// The same, by year, and only at these speeds (bits: 1 << Speed; 0: all). Counts the games
    /// from the usual start position (all a player's games, in practice).
    [[nodiscard]] PlayerStats playerStats(std::span<const std::string> names,
                                          std::uint8_t speeds = 0) const;

    /// The explorer for a line of moves from the usual start position: its games, and with
    /// positions.bin also those reaching the same position by another move order.
    [[nodiscard]] Explored explore(std::span<const core::Move> line,
                                   const ExploreFilter& filter = {}) const;
    /// The same for any position, by positions.bin: one set up from a FEN, or the end of a
    /// line from one (empty without positions.bin, or if no two games reach it early enough).
    [[nodiscard]] Explored explorePosition(const core::Position& position,
                                           const ExploreFilter& filter = {}) const;
    /// The games of that line (or position), best rated first (engine games left out: see
    /// highestHumanRating()).
    [[nodiscard]] std::vector<GameInfo> gamesAt(std::span<const core::Move> line, std::size_t limit,
                                                const ExploreFilter& filter = {}) const;
    [[nodiscard]] std::vector<GameInfo> gamesAtPosition(const core::Position& position,
                                                        std::size_t limit,
                                                        const ExploreFilter& filter = {}) const;

private:
    struct Impl;
    explicit Database(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace kiebitz::gamedb
