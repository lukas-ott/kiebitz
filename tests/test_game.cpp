#include "kiebitz/core/game.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace {

using namespace kiebitz::core;

/// Adds SAN moves after `parent` and returns the last node; fails the test on a bad move.
NodeId line(Game& game, NodeId parent, std::initializer_list<std::string_view> sans) {
    const std::vector<std::string_view> moves(sans);
    auto result = game.addLine(parent, moves);
    EXPECT_TRUE(result.ok()) << (result.ok() ? "" : result.error().message);
    return result.ok() ? *result : kNoNode;
}

std::vector<std::string> sansOf(const Game& game, const std::vector<NodeId>& ids) {
    std::vector<std::string> result;
    result.reserve(ids.size());
    for (const NodeId id : ids) {
        result.push_back(game.node(id).san);
    }
    return result;
}

TEST(Game, NewGame) {
    const Game game;
    EXPECT_EQ(game.nodeCount(), 1U);
    EXPECT_EQ(game.mainChild(Game::root()), kNoNode);
    EXPECT_EQ(game.header("Result"), "*");
    EXPECT_EQ(game.header("White"), "?");
    EXPECT_EQ(game.header("FEN"), "");
    EXPECT_EQ(game.positionAt(Game::root()).fen(), kStartFen);
}

TEST(Game, MainLine) {
    Game game;
    const NodeId last = line(game, Game::root(), {"e4", "e5", "Nf3", "Nc6", "Bb5"});
    EXPECT_EQ(game.nodeCount(), 6U);
    const auto main = game.mainLine();
    ASSERT_EQ(main.size(), 6U);
    EXPECT_EQ(main.back(), last);
    EXPECT_EQ(sansOf(game, {main.begin() + 1, main.end()}),
              (std::vector<std::string>{"e4", "e5", "Nf3", "Nc6", "Bb5"}));
    EXPECT_EQ(game.positionAt(last).fen(),
              "r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3");
    EXPECT_EQ(game.ply(last), 5);
    EXPECT_EQ(game.moveNumber(last), 3);
    EXPECT_EQ(game.mover(last), Color::White);
    EXPECT_EQ(game.moveNumber(game.parent(last)), 2);
    EXPECT_EQ(game.mover(game.parent(last)), Color::Black);
    EXPECT_TRUE(game.isMainLine(last));
}

TEST(Game, AddingAnExistingMoveReturnsIt) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    EXPECT_EQ(*game.addSan(Game::root(), "e4"), e4);
    EXPECT_EQ(game.nodeCount(), 2U);
}

TEST(Game, Variations) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    const NodeId e5 = line(game, e4, {"e5"});
    const NodeId c5 = line(game, e4, {"c5"});
    const NodeId e6 = line(game, e4, {"e6"});
    EXPECT_EQ(game.children(e4), (std::vector<NodeId>{e5, c5, e6}));
    EXPECT_EQ(game.mainChild(e4), e5);
    EXPECT_EQ(game.nextSibling(e5), c5);
    EXPECT_FALSE(game.isMainLine(c5));
    EXPECT_TRUE(game.isMainLine(e5));

    game.promote(e6);
    EXPECT_EQ(game.children(e4), (std::vector<NodeId>{e5, e6, c5}));
    game.promote(e6);
    EXPECT_EQ(game.children(e4), (std::vector<NodeId>{e6, e5, c5}));
    game.promote(e6); // already first: no change
    EXPECT_EQ(game.children(e4), (std::vector<NodeId>{e6, e5, c5}));
    EXPECT_TRUE(game.isMainLine(e6));
}

TEST(Game, MakeMainLineWorksAllTheWayUp) {
    Game game;
    line(game, Game::root(), {"e4", "e5", "Nf3"});
    const NodeId d4 = line(game, Game::root(), {"d4"});
    const NodeId d5 = line(game, d4, {"d5"});
    const NodeId nf6 = line(game, d4, {"Nf6"});
    const NodeId c4 = line(game, nf6, {"c4"});
    EXPECT_FALSE(game.isMainLine(c4));

    game.makeMainLine(c4);
    EXPECT_TRUE(game.isMainLine(c4));
    EXPECT_EQ(sansOf(game, game.mainLine()), (std::vector<std::string>{"", "d4", "Nf6", "c4"}));
    EXPECT_EQ(game.children(d4), (std::vector<NodeId>{nf6, d5}));
}

TEST(Game, RemoveDeletesTheSubtreeAndReusesIds) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    line(game, e4, {"e5", "Nf3", "Nc6"});
    const NodeId c5 = line(game, e4, {"c5"});
    line(game, c5, {"Nf3", "d6"});
    EXPECT_EQ(game.nodeCount(), 8U); // root, e4, 3 main-line moves, 3 in the variation

    game.remove(c5);
    EXPECT_EQ(game.nodeCount(), 5U);
    EXPECT_EQ(game.children(e4).size(), 1U);

    const NodeId reused = line(game, e4, {"d5"});
    EXPECT_LT(reused, 8U); // took a freed slot instead of growing the array
    EXPECT_EQ(game.node(reused).san, "d5");
    EXPECT_EQ(game.node(reused).comment, "");
}

TEST(Game, RemoveFirstChildPromotesTheNext) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    const NodeId d4 = line(game, Game::root(), {"d4"});
    game.remove(e4);
    EXPECT_EQ(game.mainChild(Game::root()), d4);
}

TEST(Game, RemoveVariationRemovesFromTheBranchPoint) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    line(game, e4, {"e5", "Nf3"});
    const NodeId deep = line(game, e4, {"c5", "Nf3", "d6", "d4"});
    EXPECT_EQ(game.nodeCount(), 8U);

    game.removeVariation(deep); // deletes 1...c5 and everything after it
    EXPECT_EQ(game.nodeCount(), 4U);
    EXPECT_EQ(game.children(e4).size(), 1U);

    // On the main line it deletes from the node onwards.
    const NodeId nf3 = game.mainLine().back();
    game.removeVariation(nf3);
    EXPECT_EQ(game.mainLine().size(), 3U);
}

TEST(Game, IllegalSanIsReportedWithMoveNumber) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    const auto bad = game.addSan(e4, "Ke2");
    ASSERT_FALSE(bad.ok());

    const std::array<std::string_view, 3> moves{"e5", "Nf3", "Bxe5"};
    const auto result = game.addLine(e4, moves);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().message, "2... \"Bxe5\" is not a legal move here.");
    // The moves before the bad one were added.
    EXPECT_EQ(game.mainLine().size(), 4U);
}

TEST(Game, FromFenWithBlackToMove) {
    auto start = Position::fromFen("4k3/8/8/8/8/8/4P3/4K3 b - - 0 30");
    ASSERT_TRUE(start.ok());
    Game game(*start);
    EXPECT_EQ(game.header("SetUp"), "1");
    EXPECT_EQ(game.header("FEN"), "4k3/8/8/8/8/8/4P3/4K3 b - - 0 30");

    const NodeId kd7 = line(game, Game::root(), {"Kd7"});
    const NodeId e4 = line(game, kd7, {"e4"});
    EXPECT_EQ(game.moveNumber(kd7), 30);
    EXPECT_EQ(game.mover(kd7), Color::Black);
    EXPECT_EQ(game.moveNumber(e4), 31);
    EXPECT_EQ(game.mover(e4), Color::White);
}

TEST(Game, NotationOrderPutsVariationsBeforeTheNextMove) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    const NodeId e5 = line(game, e4, {"e5"});
    const NodeId nf3 = line(game, e5, {"Nf3"});
    const NodeId c5 = line(game, e4, {"c5"});
    const NodeId c5nf3 = line(game, c5, {"Nf3"});
    const NodeId d4 = line(game, Game::root(), {"d4"});
    // 1. e4 (1. d4) 1... e5 (1... c5 2. Nf3) 2. Nf3
    EXPECT_EQ(game.notationOrder(),
              (std::vector<NodeId>{Game::root(), e4, d4, e5, c5, c5nf3, nf3}));
}

TEST(Game, NextTranspositionGoesRoundTheTree) {
    Game game;
    const NodeId italian = line(game, Game::root(), {"e4", "e5", "Nf3", "Nc6"});
    const NodeId viaNf3 = line(game, Game::root(), {"Nf3", "Nc6", "e4", "e5"});
    const NodeId e4 = game.mainChild(Game::root());
    const NodeId viaNc6 = line(game, e4, {"Nc6", "Nf3", "e5"});
    // Three ways into one position, in notation order viaNf3, viaNc6, italian: each jump goes
    // to the next one, then round to the top.
    EXPECT_EQ(game.nextTransposition(viaNf3), viaNc6);
    EXPECT_EQ(game.nextTransposition(viaNc6), italian);
    EXPECT_EQ(game.nextTransposition(italian), viaNf3);
    // A position reached only once has none.
    EXPECT_EQ(game.nextTransposition(game.parent(italian)), kNoNode);
    std::vector<NodeId> transposed = game.transposedNodes();
    std::sort(transposed.begin(), transposed.end());
    // Also a move earlier: 1. Nf3 Nc6 2. e4 and 1. e4 Nc6 2. Nf3.
    std::vector<NodeId> expected{italian, viaNf3, viaNc6, game.parent(viaNf3), game.parent(viaNc6)};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(transposed, expected);
    EXPECT_EQ(game.nextTransposition(Game::root()), kNoNode);
}

TEST(Game, Annotations) {
    Game game;
    const NodeId e4 = line(game, Game::root(), {"e4"});
    game.setComment(e4, "Best by test");
    game.setCommentBefore(e4, "The main line:");
    game.setNags(e4, {nag::kGood});
    game.setArrows(e4,
                   {{Square::parse("e2").value(), Square::parse("e4").value(), MarkColor::Green}});
    game.setSquareMarks(e4, {{Square::parse("d5").value(), MarkColor::Red}});

    const Node& node = game.node(e4);
    EXPECT_EQ(node.comment, "Best by test");
    EXPECT_EQ(node.commentBefore, "The main line:");
    EXPECT_EQ(node.nags, (std::vector<std::uint8_t>{nag::kGood}));
    EXPECT_EQ(node.arrows.size(), 1U);
    EXPECT_EQ(node.squareMarks.size(), 1U);
}

TEST(Game, Headers) {
    Game game;
    game.setHeader("White", "Carlsen, Magnus");
    game.setHeader("ECO", "C65");
    EXPECT_EQ(game.header("White"), "Carlsen, Magnus");
    EXPECT_EQ(game.header("ECO"), "C65");
    EXPECT_EQ(game.headers().front().first, "Event"); // order kept
    game.removeHeader("ECO");
    EXPECT_EQ(game.header("ECO"), "");
}

TEST(Game, CopyIsIndependent) {
    Game game;
    line(game, Game::root(), {"e4", "e5"});
    Game copy = game;
    line(copy, copy.mainLine().back(), {"Nf3"});
    EXPECT_EQ(game.mainLine().size(), 3U);
    EXPECT_EQ(copy.mainLine().size(), 4U);
}

} // namespace

namespace {

using namespace kiebitz::core;

NodeId play(Game& game, NodeId from, std::initializer_list<std::string_view> sans) {
    NodeId node = from;
    for (const auto san : sans) {
        node = game.addSan(node, san).value();
    }
    return node;
}

TEST(Game, SwapsWhatFollowsATransposition) {
    // The main line's analysis goes on after 1. e4 e5 2. Nf3 Nc6; 1. Nf3 Nc6 2. e4 e5 stops
    // there. Swapped, the analysis follows 1. Nf3 and 1. e4 stops.
    Game game;
    const NodeId italian = play(game, Game::root(), {"e4", "e5", "Nf3", "Nc6"});
    const NodeId bc4 = play(game, italian, {"Bc4"});
    play(game, bc4, {"Bc5"});
    play(game, italian, {"Bb5"});
    game.setComment(bc4, "the Italian");
    const NodeId viaNf3 = play(game, Game::root(), {"Nf3", "Nc6", "e4", "e5"});
    ASSERT_TRUE(game.children(viaNf3).empty());

    EXPECT_TRUE(game.swapContinuations(italian, viaNf3));
    EXPECT_TRUE(game.children(italian).empty());
    const auto moved = game.children(viaNf3);
    ASSERT_EQ(moved.size(), 2U);
    EXPECT_EQ(game.node(moved[0]).san, "Bc4");
    EXPECT_EQ(game.node(moved[1]).san, "Bb5");
    EXPECT_EQ(game.parent(moved[0]), viaNf3);
    EXPECT_EQ(game.node(moved[0]).comment, "the Italian");
    EXPECT_EQ(game.positionAt(game.mainChild(moved[0])).fen(),
              "r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 2 4");

    // Not the same position, or one after the other: refused.
    EXPECT_FALSE(game.swapContinuations(italian, game.parent(viaNf3)));
    EXPECT_FALSE(game.swapContinuations(viaNf3, moved[0]));
    EXPECT_FALSE(game.swapContinuations(italian, italian));
}

TEST(Game, StartsAtAPositionKeepingWhatFollows) {
    Game game;
    game.setHeader("White", "Me");
    const NodeId nc6 = play(game, Game::root(), {"e4", "e5", "Nf3", "Nc6"});
    game.setComment(nc6, "the main position");
    const NodeId bb5 = play(game, nc6, {"Bb5"});
    play(game, bb5, {"a6"});
    play(game, nc6, {"Bc4"});
    play(game, Game::root(), {"d4"}); // outside: left out
    game.setNags(bb5, {1});

    const Game from = game.startingAt(nc6);
    EXPECT_EQ(from.startPosition().fen(),
              "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");
    EXPECT_EQ(from.header("White"), "Me");
    EXPECT_EQ(from.header("SetUp"), "1");
    EXPECT_EQ(from.header("FEN"), from.startPosition().fen());
    EXPECT_EQ(from.node(Game::root()).comment, "the main position");
    const auto first = from.children(Game::root());
    ASSERT_EQ(first.size(), 2U);
    EXPECT_EQ(from.node(first[0]).san, "Bb5");
    EXPECT_EQ(from.node(first[0]).nags, std::vector<std::uint8_t>{1});
    EXPECT_EQ(from.node(from.mainChild(first[0])).san, "a6");
    EXPECT_EQ(from.node(first[1]).san, "Bc4");
    EXPECT_EQ(from.nodeCount(), 4U); // the start, Bb5, a6, Bc4
    EXPECT_EQ(from.moveNumber(first[0]), 3);
}

} // namespace
