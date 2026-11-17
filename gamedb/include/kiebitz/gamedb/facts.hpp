#pragma once

#include "kiebitz/gamedb/database.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace kiebitz::gamedb {

/// A player's results: won, drawn, lost (from the player's side).
struct Score {
    std::uint64_t won = 0;
    std::uint64_t drawn = 0;
    std::uint64_t lost = 0;
    [[nodiscard]] std::uint64_t games() const { return won + drawn + lost; }
    /// Points, doubled (a draw is 1).
    [[nodiscard]] std::uint64_t points2() const { return (2 * won) + drawn; }
};

/// What a player's games say about them, beyond wins and losses.
struct PlayerFacts {
    /// Their performance rating in the games with a rated opponent and a result (0: none):
    /// the opponents' average plus 400 · log10(p / (1 - p)), within ±800.
    int performance = 0;
    int averageOpponent = 0; ///< the opponents' average rating in those games
    std::uint64_t rated = 0; ///< how many games that is
    /// Their own rating from the games, the last of each month (yyyymm), oldest first.
    std::vector<std::pair<int, int>> ratings;

    struct Opponent {
        std::string name;
        Score score; ///< the player's
    };
    std::vector<Opponent> opponents; ///< those they met most, most games first (at most 5)

    struct Event {
        std::string name;
        int date = 0; ///< their last game in it (yyyymmdd)
        Score score;
    };
    /// Their tournaments, the latest first (at most 5); online sites' "Rated Blitz game" and
    /// the like aren't tournaments and are left out.
    std::vector<Event> events;

    Score againstYou; ///< the player's results against `yourNames`
};

/// Works the facts out from the games of a database. `names` are the player's (any case);
/// a game is theirs if exactly one side is called one of them. `yourNames`: yours, for
/// "against you" (empty: none). `speeds` (by game id - 1, as Database::speeds()) and `allowed`
/// (bits: 1 << Speed; 0: all) keep only games at some speeds.
[[nodiscard]] PlayerFacts playerFacts(std::span<const GameInfo> games,
                                      std::span<const std::string> names,
                                      std::span<const std::string> yourNames,
                                      std::span<const Speed> speeds = {}, std::uint8_t allowed = 0);

/// A player's rating over time at each speed: by month (yyyymm), the date of the last game
/// that month and the rating in it.
using RatingHistory = std::map<Speed, std::map<int, std::pair<int, int>>>;

/// Adds the player's ratings in these games to `history` (the latest game of a month wins).
/// `speeds` is by game id - 1 (Database::speeds()); games without a known speed count as
/// `unknownAs` (Unknown: left out).
void addRatings(RatingHistory& history, std::span<const GameInfo> games,
                std::span<const std::string> names, std::span<const Speed> speeds, Speed unknownAs);

/// One speed's ratings, oldest first, as (yyyymm, rating).
[[nodiscard]] std::vector<std::pair<int, int>> ratingLine(const RatingHistory& history,
                                                          Speed speed);

} // namespace kiebitz::gamedb
