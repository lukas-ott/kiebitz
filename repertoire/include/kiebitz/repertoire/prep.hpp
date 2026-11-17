#pragma once

#include "kiebitz/core/game.hpp"
#include "kiebitz/core/position.hpp"
#include "kiebitz/repertoire/tree.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Preparing against a player: what they play against what you could play, and a repertoire
/// file of the lines chosen.
namespace kiebitz::repertoire {

/// One of the player's games, from the usual start.
struct PlayedGame {
    std::vector<core::Move> line;
    core::Color side = core::Color::White; ///< the player's colour
    int points2 = -1; ///< the player's result, doubled: 2 won, 1 drawn, 0 lost; -1 unknown
    int date = 0;     ///< yyyymmdd (as far as known: 20240000 for "2024"), 0 unknown
};

/// A move from a position of the preparation, with what the player's games say about it.
struct PrepMove {
    core::Move move;
    std::string san;           ///< "Nf3"
    bool yours = false;        ///< your move (else theirs)
    std::uint64_t games = 0;   ///< the player's games through it
    std::uint64_t scored = 0;  ///< of those, with a known result
    std::uint64_t points2 = 0; ///< the player's points in them, doubled
    std::uint64_t won = 0;     ///< the player's wins, draws and losses in them
    std::uint64_t drawn = 0;
    std::uint64_t lost = 0;
    int lastDate = 0; ///< the newest of them (yyyymmdd), 0 unknown
    /// Your move: the repertoire's (main) move here. Their move: your repertoire answers it.
    bool inRepertoire = false;
    /// Your repertoire has this position but not this move: yours, a move it doesn't play;
    /// theirs, a move it doesn't answer.
    bool outsideRepertoire = false;

    /// The player's score in these games, 0..1, or -1 if none is known.
    [[nodiscard]] double score() const {
        return scored > 0 ? static_cast<double>(points2) / (2.0 * static_cast<double>(scored))
                          : -1.0;
    }
};

/// A line where the player scores badly, and their games through it.
struct WeakLine {
    std::vector<core::Move> line; ///< from the start
    std::uint64_t games = 0;
    std::uint64_t scored = 0;
    std::uint64_t points2 = 0;
    std::uint64_t won = 0;
    std::uint64_t drawn = 0;
    std::uint64_t lost = 0;

    /// The player's score there, 0..1.
    [[nodiscard]] double score() const {
        return scored > 0 ? static_cast<double>(points2) / (2.0 * static_cast<double>(scored))
                          : 0.0;
    }
};

/// The player's games with one colour as a tree of moves, seen from your side (you have the
/// other colour), with your repertoire for that side alongside.
class PrepTree {
public:
    /// `you`: the colour you will have; the player's games with the other colour are used.
    /// `repertoire` (optional) is your repertoire for `you`; it is copied.
    PrepTree(std::span<const PlayedGame> games, core::Color you, const Tree* repertoire = nullptr);
    ~PrepTree();
    PrepTree(const PrepTree&) = delete;
    PrepTree& operator=(const PrepTree&) = delete;
    PrepTree(PrepTree&&) noexcept;
    PrepTree& operator=(PrepTree&&) noexcept;

    [[nodiscard]] core::Color you() const noexcept { return m_you; }
    /// The player's games with their colour.
    [[nodiscard]] std::uint64_t games() const noexcept;
    /// The player's games that reached the position after `line` (from the start).
    [[nodiscard]] std::uint64_t gamesAt(std::span<const core::Move> line) const;

    /// The moves after `line`: those in the player's games, most played first; at your move
    /// also your repertoire's moves there that they have never met (0 games), after them.
    [[nodiscard]] std::vector<PrepMove> moves(std::span<const core::Move> line) const;

    /// Where the player scores worst (under 50%), worst first (then the most games): lines
    /// with at least `minGames` finished games, each as long as all those games go together
    /// (where they part, a line ends). No line is part of another; at most `count`.
    [[nodiscard]] std::vector<WeakLine> weakLines(std::uint64_t minGames = 3,
                                                  std::size_t count = 5) const;

    struct FileOptions {
        std::string event;          ///< the file's [Event]: "Preparation: Petra Pawn"
        int plies = 8;              ///< how far past each chosen line their replies go
        std::uint64_t minGames = 2; ///< their replies with fewer games are left out...
        std::size_t keep = 1;       ///< ... except the most played this many
        /// A comment for each of their moves (their games, score, when last), or none.
        std::function<std::string(const PrepMove&)> comment;
    };
    /// A repertoire game for `you`: the chosen lines merged, and from the end of each, their
    /// replies from their games (your answer: the repertoire's move, else the one most played
    /// against them), as far as `plies`. Headers: [Event], [FileType "Repertoire"] and
    /// [Orientation].
    [[nodiscard]] core::Game prepFile(std::span<const std::vector<core::Move>> lines,
                                      const FileOptions& options) const;

private:
    struct Node;
    [[nodiscard]] const Node* find(std::span<const core::Move> line) const;

    core::Color m_you;
    std::unique_ptr<Node> m_root;
    std::unique_ptr<Tree> m_repertoire;
};

/// "2024.05.12" (also "2024.??.??", "2024") as yyyymmdd (unknown parts 0), 0 if no year.
[[nodiscard]] int dateNumber(std::string_view pgnDate);

} // namespace kiebitz::repertoire
