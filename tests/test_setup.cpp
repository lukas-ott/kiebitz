// Setting up a position by hand.

#include "kiebitz/core/setup.hpp"

#include <gtest/gtest.h>

namespace {

using namespace kiebitz::core;

Square sq(const char* name) {
    return *Square::parse(name);
}

TEST(Setup, RoundTripsAPosition) {
    const BoardSetup start = BoardSetup::from(Position());
    EXPECT_EQ(start.fen(), std::string(kStartFen));
    EXPECT_TRUE(start.problems().empty());
    ASSERT_TRUE(start.position().has_value());
}

TEST(Setup, SaysWhatIsWrong) {
    BoardSetup setup = BoardSetup::empty();
    EXPECT_EQ(setup.problems(),
              (std::vector{SetupProblem::NoWhiteKing, SetupProblem::NoBlackKing}));
    setup.put(sq("e1"), {PieceType::King, Color::White});
    setup.put(sq("e8"), {PieceType::King, Color::Black});
    setup.put(sq("a8"), {PieceType::Pawn, Color::White});
    EXPECT_EQ(setup.problems(), std::vector{SetupProblem::PawnOnBackRank});
    setup.put(sq("a8"), {});
    EXPECT_TRUE(setup.problems().empty());
    setup.put(sq("d1"), {PieceType::King, Color::White});
    EXPECT_EQ(setup.problems(), std::vector{SetupProblem::TwoWhiteKings});
    setup.put(sq("d1"), {});
    // Black's king attacked with White to move: Black can't have just moved into it.
    setup.put(sq("e4"), {PieceType::Rook, Color::White});
    EXPECT_EQ(setup.problems(), std::vector{SetupProblem::OtherSideInCheck});
    EXPECT_FALSE(setup.position().has_value());
    setup.sideToMove = Color::Black;
    EXPECT_TRUE(setup.problems().empty());
    EXPECT_TRUE(setup.position().has_value());
}

TEST(Setup, CastlingAndEnPassantOnlyWhereThePiecesAllowThem) {
    BoardSetup setup = BoardSetup::empty();
    setup.put(sq("e1"), {PieceType::King, Color::White});
    setup.put(sq("h1"), {PieceType::Rook, Color::White});
    setup.put(sq("g8"), {PieceType::King, Color::Black});
    setup.castling = {true, true, true, true};
    const CastlingRights possible = setup.possibleCastling();
    EXPECT_TRUE(possible.whiteKingSide);
    EXPECT_FALSE(possible.whiteQueenSide);
    EXPECT_FALSE(possible.blackKingSide);
    EXPECT_EQ(setup.fen(), "6k1/8/8/8/8/8/8/4K2R w K - 0 1");

    // Black to move after e2-e4: e3 is the en passant square, if a black pawn can take there.
    setup.put(sq("e4"), {PieceType::Pawn, Color::White});
    setup.put(sq("d4"), {PieceType::Pawn, Color::Black});
    setup.sideToMove = Color::Black;
    setup.fullmoveNumber = 12;
    EXPECT_EQ(setup.possibleEnPassant(), std::vector{sq("e3")});
    setup.enPassant = sq("e3");
    EXPECT_EQ(setup.fen(), "6k1/8/8/8/3pP3/8/8/4K2R b K e3 0 12");
    setup.enPassant = sq("a3"); // no pawn there: left out
    EXPECT_EQ(setup.fen(), "6k1/8/8/8/3pP3/8/8/4K2R b K - 0 12");
}

} // namespace
