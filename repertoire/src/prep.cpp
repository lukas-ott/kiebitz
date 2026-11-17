#include "kiebitz/repertoire/prep.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace kiebitz::repertoire {

/// The player's games as a tree of moves, each node with what the games through it add up to.
struct PrepTree::Node {
    std::uint64_t games = 0;
    std::uint64_t scored = 0;
    std::uint64_t points2 = 0;
    std::uint64_t won = 0; ///< by the player
    std::uint64_t drawn = 0;
    std::uint64_t lost = 0;
    int lastDate = 0;
    std::vector<std::pair<core::Move, std::unique_ptr<Node>>> children;

    Node* child(core::Move move) {
        for (auto& [played, node] : children) {
            if (played == move) {
                return node.get();
            }
        }
        children.emplace_back(move, std::make_unique<Node>());
        return children.back().second.get();
    }
    void count(const PlayedGame& game) {
        ++games;
        if (game.points2 >= 0) {
            ++scored;
            points2 += static_cast<std::uint64_t>(game.points2);
            if (game.points2 == 2) {
                ++won;
            } else if (game.points2 == 1) {
                ++drawn;
            } else {
                ++lost;
            }
        }
        lastDate = std::max(lastDate, game.date);
    }
};

namespace {

constexpr std::size_t kMaxPlies = 40; ///< the opening and early middlegame: enough to prepare

} // namespace

int dateNumber(std::string_view pgnDate) {
    // "2024.05.12": up to three parts of digits; "??" counts as 0.
    std::array<int, 3> parts{0, 0, 0};
    std::size_t part = 0;
    for (const char c : pgnDate) {
        if (c == '.' || c == '-' || c == '/') {
            if (++part == 3) {
                break;
            }
        } else if (c >= '0' && c <= '9') {
            parts[part] = (parts[part] * 10) + (c - '0');
        }
    }
    if (parts[0] < 1000 || parts[0] > 9999) {
        return 0;
    }
    return (parts[0] * 10000) + (std::min(parts[1], 12) * 100) + std::min(parts[2], 31);
}

PrepTree::PrepTree(std::span<const PlayedGame> games, core::Color you, const Tree* repertoire)
    : m_you(you), m_root(std::make_unique<Node>()),
      m_repertoire(repertoire != nullptr ? std::make_unique<Tree>(*repertoire) : nullptr) {
    for (const PlayedGame& game : games) {
        if (game.side == you) {
            continue; // their games with your colour: not what you will meet
        }
        m_root->count(game);
        Node* node = m_root.get();
        for (std::size_t ply = 0; ply < game.line.size() && ply < kMaxPlies; ++ply) {
            node = node->child(game.line[ply]);
            node->count(game);
        }
    }
}

PrepTree::~PrepTree() = default;
PrepTree::PrepTree(PrepTree&&) noexcept = default;
PrepTree& PrepTree::operator=(PrepTree&&) noexcept = default;

std::uint64_t PrepTree::games() const noexcept {
    return m_root->games;
}

const PrepTree::Node* PrepTree::find(std::span<const core::Move> line) const {
    const Node* node = m_root.get();
    for (const core::Move move : line) {
        const Node* next = nullptr;
        for (const auto& [played, child] : node->children) {
            if (played == move) {
                next = child.get();
            }
        }
        if (next == nullptr) {
            return nullptr;
        }
        node = next;
    }
    return node;
}

std::uint64_t PrepTree::gamesAt(std::span<const core::Move> line) const {
    const Node* node = find(line);
    return node != nullptr ? node->games : 0;
}

std::vector<WeakLine> PrepTree::weakLines(std::uint64_t minGames, std::size_t count) const {
    // Every position with enough finished games and a bad score, as deep as its games go
    // together: a position whose games all go on with one move isn't a line's end.
    std::vector<WeakLine> found;
    std::vector<core::Move> line;
    const auto walk = [&](const auto& self, const Node& node) -> void {
        const bool together =
            std::any_of(node.children.begin(), node.children.end(),
                        [&](const auto& child) { return child.second->games == node.games; });
        if (!line.empty() && !together && node.scored >= minGames &&
            node.points2 < node.scored) { // under half the points
            WeakLine weak;
            weak.line = line;
            weak.games = node.games;
            weak.scored = node.scored;
            weak.points2 = node.points2;
            weak.won = node.won;
            weak.drawn = node.drawn;
            weak.lost = node.lost;
            found.push_back(std::move(weak));
        }
        for (const auto& [move, child] : node.children) {
            if (child->scored < minGames) {
                continue; // nothing deeper has enough either
            }
            line.push_back(move);
            self(self, *child);
            line.pop_back();
        }
    };
    walk(walk, *m_root);
    std::stable_sort(found.begin(), found.end(), [](const WeakLine& a, const WeakLine& b) {
        // a.points2 / a.scored < b.points2 / b.scored, without dividing.
        const std::uint64_t left = a.points2 * b.scored;
        const std::uint64_t right = b.points2 * a.scored;
        return left != right ? left < right : a.games > b.games;
    });
    // The worst first; a line inside one taken (or holding one) adds nothing.
    const auto within = [](const std::vector<core::Move>& shorter,
                           const std::vector<core::Move>& longer) {
        return shorter.size() <= longer.size() &&
               std::equal(shorter.begin(), shorter.end(), longer.begin());
    };
    std::vector<WeakLine> result;
    for (WeakLine& weak : found) {
        if (result.size() >= count) {
            break;
        }
        const bool overlaps = std::any_of(result.begin(), result.end(), [&](const WeakLine& taken) {
            return within(taken.line, weak.line) || within(weak.line, taken.line);
        });
        if (!overlaps) {
            result.push_back(std::move(weak));
        }
    }
    return result;
}

std::vector<PrepMove> PrepTree::moves(std::span<const core::Move> line) const {
    core::Position position;
    for (const core::Move move : line) {
        position.play(move);
    }
    const bool yours = position.sideToMove() == m_you;
    const Entry* entry = m_repertoire ? m_repertoire->find(position.hash()) : nullptr;
    const auto repertoireMove = [&](core::Move move) {
        return entry != nullptr &&
               std::any_of(entry->moves.begin(), entry->moves.end(),
                           [&](const Continuation& c) { return c.main && c.move == move; });
    };
    const bool repertoireHasMove =
        entry != nullptr && std::any_of(entry->moves.begin(), entry->moves.end(),
                                        [](const Continuation& c) { return c.main; });
    std::vector<PrepMove> moves;
    if (const Node* node = find(line)) {
        for (const auto& [move, child] : node->children) {
            PrepMove prep;
            prep.move = move;
            prep.san = position.san(move);
            prep.yours = yours;
            prep.games = child->games;
            prep.scored = child->scored;
            prep.points2 = child->points2;
            prep.won = child->won;
            prep.drawn = child->drawn;
            prep.lost = child->lost;
            prep.lastDate = child->lastDate;
            if (yours) {
                prep.inRepertoire = repertoireMove(move);
                prep.outsideRepertoire = repertoireHasMove && !prep.inRepertoire;
            } else if (entry != nullptr) {
                // Theirs: answered if the repertoire has a move of yours after it.
                core::Position after = position;
                after.play(move);
                const Entry* next = m_repertoire->find(after.hash());
                prep.inRepertoire =
                    next != nullptr && std::any_of(next->moves.begin(), next->moves.end(),
                                                   [](const Continuation& c) { return c.main; });
                prep.outsideRepertoire = !prep.inRepertoire;
            }
            moves.push_back(std::move(prep));
        }
    }
    std::stable_sort(moves.begin(), moves.end(),
                     [](const PrepMove& a, const PrepMove& b) { return a.games > b.games; });
    // Your repertoire's moves they have never met.
    if (yours && entry != nullptr) {
        for (const Continuation& continuation : entry->moves) {
            if (!continuation.main ||
                std::any_of(moves.begin(), moves.end(),
                            [&](const PrepMove& m) { return m.move == continuation.move; })) {
                continue;
            }
            PrepMove prep;
            prep.move = continuation.move;
            prep.san = position.san(continuation.move);
            prep.yours = true;
            prep.inRepertoire = true;
            moves.push_back(std::move(prep));
        }
    }
    return moves;
}

core::Game PrepTree::prepFile(std::span<const std::vector<core::Move>> lines,
                              const FileOptions& options) const {
    core::Game game;
    game.setHeader("Event", options.event);
    game.setHeader("FileType", "Repertoire");
    game.setHeader("Orientation", m_you == core::Color::White ? "white" : "black");
    const auto comment = [&](core::NodeId node, const PrepMove& move) {
        if (options.comment && move.games > 0) {
            game.setComment(node, options.comment(move));
        }
    };
    // From the end of a chosen line: their replies (the ones they play), your answers.
    const auto extend = [&](const auto& self, core::NodeId node, std::vector<core::Move>& line,
                            int left) -> void {
        if (left <= 0) {
            return;
        }
        const std::vector<PrepMove> next = moves(line);
        if (next.empty()) {
            return;
        }
        if (next.front().yours) {
            // Your answer: the repertoire's, else the one most played against them.
            const PrepMove* pick = nullptr;
            for (const PrepMove& move : next) {
                if (move.inRepertoire && (pick == nullptr || move.games > pick->games)) {
                    pick = &move;
                }
            }
            if (pick == nullptr && next.front().games > 0) {
                pick = &next.front();
            }
            if (pick == nullptr) {
                return;
            }
            const core::NodeId added = game.addMove(node, pick->move);
            line.push_back(pick->move);
            self(self, added, line, left - 1);
            line.pop_back();
            return;
        }
        for (std::size_t k = 0; k < next.size(); ++k) {
            const PrepMove& move = next[k];
            if (move.games == 0 || (k >= options.keep && move.games < options.minGames)) {
                continue;
            }
            const core::NodeId added = game.addMove(node, move.move);
            comment(added, move);
            line.push_back(move.move);
            self(self, added, line, left - 1);
            line.pop_back();
        }
    };
    for (const std::vector<core::Move>& chosen : lines) {
        core::NodeId node = core::Game::root();
        std::vector<core::Move> line;
        for (const core::Move move : chosen) {
            const std::vector<PrepMove> here = moves(line);
            node = game.addMove(node, move);
            for (const PrepMove& prep : here) {
                if (prep.move == move && !prep.yours) {
                    comment(node, prep); // their move on the way: how often they play it
                }
            }
            line.push_back(move);
        }
        extend(extend, node, line, options.plies);
    }
    return game;
}

} // namespace kiebitz::repertoire
