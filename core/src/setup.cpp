#include "kiebitz/core/setup.hpp"

#include <algorithm>
#include <array>

namespace kiebitz::core {
namespace {

bool is(const BoardSetup& setup, int file, int rank, PieceType type, Color color) {
    const Piece piece = setup.at(Square::at(file, rank));
    return piece.type == type && piece.color == color;
}

} // namespace

BoardSetup BoardSetup::from(const Position& position) {
    BoardSetup setup;
    for (int i = 0; i < 64; ++i) {
        setup.board[static_cast<std::size_t>(i)] = position.pieceAt(Square::fromIndex(i));
    }
    setup.sideToMove = position.sideToMove();
    setup.castling = position.castlingRights();
    setup.enPassant = position.enPassantSquare();
    setup.fullmoveNumber = position.fullmoveNumber();
    return setup;
}

CastlingRights BoardSetup::possibleCastling() const {
    CastlingRights rights;
    const bool whiteKing = is(*this, 4, 0, PieceType::King, Color::White);
    const bool blackKing = is(*this, 4, 7, PieceType::King, Color::Black);
    rights.whiteKingSide = whiteKing && is(*this, 7, 0, PieceType::Rook, Color::White);
    rights.whiteQueenSide = whiteKing && is(*this, 0, 0, PieceType::Rook, Color::White);
    rights.blackKingSide = blackKing && is(*this, 7, 7, PieceType::Rook, Color::Black);
    rights.blackQueenSide = blackKing && is(*this, 0, 7, PieceType::Rook, Color::Black);
    return rights;
}

std::vector<Square> BoardSetup::possibleEnPassant() const {
    // The side that just moved is the one not to move; its pawn went two squares.
    const Color moved = opposite(sideToMove);
    const int pawnRank = moved == Color::White ? 3 : 4;   // the 4th rank from its side
    const int passedRank = moved == Color::White ? 2 : 5; // the square it passed
    const int fromRank = moved == Color::White ? 1 : 6;   // where it came from
    std::vector<Square> squares;
    for (int file = 0; file < 8; ++file) {
        if (is(*this, file, pawnRank, PieceType::Pawn, moved) &&
            at(Square::at(file, passedRank)).isNone() && at(Square::at(file, fromRank)).isNone()) {
            squares.push_back(Square::at(file, passedRank));
        }
    }
    return squares;
}

std::string BoardSetup::fen() const {
    std::string fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const Piece piece = at(Square::at(file, rank));
            if (piece.isNone()) {
                ++empty;
                continue;
            }
            if (empty > 0) {
                fen += static_cast<char>('0' + empty);
                empty = 0;
            }
            fen += piece.fenChar();
        }
        if (empty > 0) {
            fen += static_cast<char>('0' + empty);
        }
        if (rank > 0) {
            fen += '/';
        }
    }
    fen += sideToMove == Color::White ? " w " : " b ";
    const CastlingRights possible = possibleCastling();
    std::string castles;
    if (castling.whiteKingSide && possible.whiteKingSide) {
        castles += 'K';
    }
    if (castling.whiteQueenSide && possible.whiteQueenSide) {
        castles += 'Q';
    }
    if (castling.blackKingSide && possible.blackKingSide) {
        castles += 'k';
    }
    if (castling.blackQueenSide && possible.blackQueenSide) {
        castles += 'q';
    }
    fen += castles.empty() ? "-" : castles;
    const auto candidates = possibleEnPassant();
    if (enPassant &&
        std::find(candidates.begin(), candidates.end(), *enPassant) != candidates.end()) {
        fen += ' ' + enPassant->name();
    } else {
        fen += " -";
    }
    fen += " 0 " + std::to_string(std::max(1, fullmoveNumber));
    return fen;
}

std::vector<SetupProblem> BoardSetup::problems() const {
    std::vector<SetupProblem> problems;
    std::array<int, 2> kings{};
    std::array<int, 2> pieces{};
    bool backRankPawn = false;
    for (int i = 0; i < 64; ++i) {
        const Piece piece = board[static_cast<std::size_t>(i)];
        if (piece.isNone()) {
            continue;
        }
        const auto side = static_cast<std::size_t>(piece.color);
        ++pieces[side];
        if (piece.type == PieceType::King) {
            ++kings[side];
        }
        const int rank = Square::fromIndex(i).rank();
        if (piece.type == PieceType::Pawn && (rank == 0 || rank == 7)) {
            backRankPawn = true;
        }
    }
    if (kings[0] == 0) {
        problems.push_back(SetupProblem::NoWhiteKing);
    }
    if (kings[1] == 0) {
        problems.push_back(SetupProblem::NoBlackKing);
    }
    if (kings[0] > 1) {
        problems.push_back(SetupProblem::TwoWhiteKings);
    }
    if (kings[1] > 1) {
        problems.push_back(SetupProblem::TwoBlackKings);
    }
    if (backRankPawn) {
        problems.push_back(SetupProblem::PawnOnBackRank);
    }
    if (pieces[0] > 16) {
        problems.push_back(SetupProblem::TooManyWhitePieces);
    }
    if (pieces[1] > 16) {
        problems.push_back(SetupProblem::TooManyBlackPieces);
    }
    if (problems.empty() && !Position::fromFen(fen())) {
        // Everything else is in order, so the only thing left a FEN can fail on.
        problems.push_back(SetupProblem::OtherSideInCheck);
    }
    return problems;
}

std::optional<Position> BoardSetup::position() const {
    if (!problems().empty()) {
        return std::nullopt;
    }
    auto position = Position::fromFen(fen());
    if (!position) {
        return std::nullopt;
    }
    return std::move(*position);
}

} // namespace kiebitz::core
