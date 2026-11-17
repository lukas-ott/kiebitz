#include "kiebitz/gamedb/facts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_set>

namespace kiebitz::gamedb {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return result;
}

/// "2024.05.12" (or "2024.??.??", "2024") as yyyymmdd, unknown parts 0; 0 without a year.
int dateNumber(std::string_view date) {
    const auto part = [&](std::size_t from, std::size_t length) {
        int value = 0;
        for (std::size_t i = from; i < from + length; ++i) {
            if (i >= date.size() || date[i] < '0' || date[i] > '9') {
                return 0;
            }
            value = (value * 10) + (date[i] - '0');
        }
        return value;
    };
    const int year = part(0, 4);
    return year == 0 ? 0 : (year * 10000) + (part(5, 2) * 100) + part(8, 2);
}

/// Online sites name every game, not a tournament: "Rated Blitz game", "Live Chess".
bool isTournament(std::string_view event) {
    if (event.empty() || event == "?" || event == "-") {
        return false;
    }
    constexpr std::array<std::string_view, 4> kGeneric{"Rated ", "Casual ", "Live Chess",
                                                       "Let's Play"};
    return std::none_of(kGeneric.begin(), kGeneric.end(),
                        [&](std::string_view generic) { return event.starts_with(generic); });
}

void count(Score& score, int points2) {
    if (points2 == 2) {
        ++score.won;
    } else if (points2 == 1) {
        ++score.drawn;
    } else {
        ++score.lost;
    }
}

} // namespace

PlayerFacts playerFacts(std::span<const GameInfo> games, std::span<const std::string> names,
                        std::span<const std::string> yourNames, std::span<const Speed> speeds,
                        std::uint8_t allowed) {
    std::unordered_set<std::string> theirs;
    for (const std::string& name : names) {
        theirs.insert(lower(name));
    }
    std::unordered_set<std::string> yours;
    for (const std::string& name : yourNames) {
        yours.insert(lower(name));
    }

    PlayerFacts facts;
    std::uint64_t opponentsRating = 0;
    std::uint64_t ratedPoints2 = 0;
    std::map<int, std::pair<int, int>> monthly; ///< yyyymm → (date, rating) of the last game
    std::map<std::string, std::pair<std::string, Score>> opponents; ///< lower case → name
    std::map<std::string, PlayerFacts::Event> events;
    for (const GameInfo& game : games) {
        const auto index = static_cast<std::size_t>(game.id - 1);
        if (allowed != 0) {
            const Speed speed = index < speeds.size() ? speeds[index] : Speed::Unknown;
            if ((allowed & (1U << static_cast<unsigned>(speed))) == 0) {
                continue;
            }
        }
        const bool asWhite = theirs.contains(lower(game.white));
        const bool asBlack = theirs.contains(lower(game.black));
        if (asWhite == asBlack) {
            continue; // neither (or both): not theirs
        }
        const std::string& opponent = asWhite ? game.black : game.white;
        const int ownRating = asWhite ? game.whiteElo : game.blackElo;
        const int opponentRating = asWhite ? game.blackElo : game.whiteElo;
        const int date = dateNumber(game.date);
        if (ownRating > 0 && date > 0) {
            auto& last = monthly[date / 100];
            if (date >= last.first) {
                last = {date, ownRating};
            }
        }
        if (game.result == GameResult::Unknown) {
            continue;
        }
        int points2 = 1;
        if (game.result == GameResult::WhiteWins) {
            points2 = asWhite ? 2 : 0;
        } else if (game.result == GameResult::BlackWins) {
            points2 = asWhite ? 0 : 2;
        }
        if (opponentRating > 0) {
            ++facts.rated;
            opponentsRating += static_cast<std::uint64_t>(opponentRating);
            ratedPoints2 += static_cast<std::uint64_t>(points2);
        }
        if (!opponent.empty() && opponent != "?") {
            auto& [name, score] = opponents[lower(opponent)];
            if (name.empty()) {
                name = opponent;
            }
            count(score, points2);
        }
        if (isTournament(game.event)) {
            PlayerFacts::Event& event = events[game.event];
            event.name = game.event;
            event.date = std::max(event.date, date);
            count(event.score, points2);
        }
        if (yours.contains(lower(opponent))) {
            count(facts.againstYou, points2);
        }
    }

    if (facts.rated > 0) {
        const auto rated = static_cast<double>(facts.rated);
        facts.averageOpponent =
            static_cast<int>(std::lround(static_cast<double>(opponentsRating) / rated));
        const double p = static_cast<double>(ratedPoints2) / (2.0 * rated);
        double difference = 800.0;
        if (p <= 0.0) {
            difference = -800.0;
        } else if (p < 1.0) {
            difference = std::clamp(400.0 * std::log10(p / (1.0 - p)), -800.0, 800.0);
        }
        facts.performance = facts.averageOpponent + static_cast<int>(std::lround(difference));
    }
    for (const auto& [month, last] : monthly) {
        facts.ratings.emplace_back(month, last.second);
    }
    for (auto& [key, opponent] : opponents) {
        facts.opponents.push_back({std::move(opponent.first), opponent.second});
    }
    std::stable_sort(
        facts.opponents.begin(), facts.opponents.end(),
        [](const auto& a, const auto& b) { return a.score.games() > b.score.games(); });
    if (facts.opponents.size() > 5) {
        facts.opponents.resize(5);
    }
    for (auto& [name, event] : events) {
        facts.events.push_back(std::move(event));
    }
    std::stable_sort(facts.events.begin(), facts.events.end(),
                     [](const auto& a, const auto& b) { return a.date > b.date; });
    if (facts.events.size() > 5) {
        facts.events.resize(5);
    }
    return facts;
}

void addRatings(RatingHistory& history, std::span<const GameInfo> games,
                std::span<const std::string> names, std::span<const Speed> speeds,
                Speed unknownAs) {
    std::unordered_set<std::string> theirs;
    for (const std::string& name : names) {
        theirs.insert(lower(name));
    }
    for (const GameInfo& game : games) {
        const bool asWhite = theirs.contains(lower(game.white));
        const bool asBlack = theirs.contains(lower(game.black));
        const int rating = asWhite ? game.whiteElo : game.blackElo;
        const int date = dateNumber(game.date);
        if (asWhite == asBlack || rating <= 0 || date == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(game.id - 1);
        Speed speed = index < speeds.size() ? speeds[index] : Speed::Unknown;
        if (speed == Speed::Unknown) {
            speed = unknownAs;
        }
        if (speed == Speed::Unknown) {
            continue;
        }
        auto& last = history[speed][date / 100];
        if (date >= last.first) {
            last = {date, rating};
        }
    }
}

std::vector<std::pair<int, int>> ratingLine(const RatingHistory& history, Speed speed) {
    std::vector<std::pair<int, int>> line;
    if (const auto found = history.find(speed); found != history.end()) {
        for (const auto& [month, last] : found->second) {
            line.emplace_back(month, last.second);
        }
    }
    return line;
}

} // namespace kiebitz::gamedb
