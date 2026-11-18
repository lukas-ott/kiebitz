#pragma once

#include "kiebitz/core/position.hpp"
#include "kiebitz/core/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

/// Setting up a position by hand: a board being edited, which may not be a legal position yet,
/// and what is still wrong with it.
namespace kiebitz::core {

/// What keeps a set-up position from being played. The UI words them; the order is the order
/// they are reported in.
enum class SetupProblem : std::uint8_t {
    NoWhiteKing,
    NoBlackKing,
    TwoWhiteKings, ///< or more
    TwoBlackKings,
    PawnOnBackRank,     ///< a pawn on the first or eighth rank
    TooManyWhitePieces, ///< more than 16
    TooManyBlackPieces,
    OtherSideInCheck, ///< the side not to move is in check: it can't be its opponent's turn
};

/// A board being set up: pieces anywhere, the side to move, castling rights, en passant and the
/// move number. Castling and en passant that the pieces don't allow are left out of the FEN.
struct BoardSetup {
    std::array<Piece, 64> board{}; ///< by Square::index()
    Color sideToMove = Color::White;
    CastlingRights castling;
    std::optional<Square> enPassant;
    int fullmoveNumber = 1;

    /// An empty board, White to move.
    static BoardSetup empty() { return {}; }
    /// The pieces and settings of a position.
    static BoardSetup from(const Position& position);

    [[nodiscard]] Piece at(Square square) const { return board[square.index()]; }
    void put(Square square, Piece piece) { board[square.index()] = piece; }

    /// The castling rights the pieces allow (king and rook on their home squares).
    [[nodiscard]] CastlingRights possibleCastling() const;
    /// The squares an en passant capture could be on: behind a pawn of the side that just moved
    /// that stands on its fourth rank (from its side), with both squares behind it empty.
    [[nodiscard]] std::vector<Square> possibleEnPassant() const;

    /// The FEN, with only the castling and en passant the pieces allow.
    [[nodiscard]] std::string fen() const;
    /// Everything that is still wrong, in a fixed order (empty: it can be played).
    [[nodiscard]] std::vector<SetupProblem> problems() const;
    /// The position, if it can be played.
    [[nodiscard]] std::optional<Position> position() const;
};

} // namespace kiebitz::core
