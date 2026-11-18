#pragma once

#include "BoardTheme.hpp"
#include "PieceSet.hpp"
#include "kiebitz/core/setup.hpp"

#include <QDialog>
#include <QWidget>

#include <array>
#include <optional>

class QBoxLayout;
class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace kiebitz::app {

class AppStyle;
class Stepper;

/// The board of the position editor: pieces go where you click with the piece chosen in the
/// palette, a piece on the board can be dragged elsewhere (off the board: removed), and a
/// right-click removes one.
class SetupBoard : public QWidget {
    Q_OBJECT

public:
    SetupBoard(BoardTheme theme, const QString& pieceSet, QWidget* parent = nullptr);

    void setSetup(const core::BoardSetup& setup);
    [[nodiscard]] const core::BoardSetup& setup() const noexcept { return m_setup; }
    /// The piece a click puts down; nullopt: none (clicks only pick pieces up), None: removes.
    void setTool(std::optional<core::Piece> tool) { m_tool = tool; }
    /// A piece carried from the palette, drawn under the mouse at `at` (in this widget) while
    /// over the board; nullopt: none.
    void carry(std::optional<core::Piece> piece, QPoint at = {});
    /// Puts `piece` on the square under `at` (in this widget). False if `at` is off the board.
    bool drop(core::Piece piece, QPoint at);
    void setFlipped(bool flipped);
    [[nodiscard]] bool isFlipped() const noexcept { return m_flipped; }
    /// The middle of a square, in this widget (tests click there).
    [[nodiscard]] QPoint squareCenter(core::Square square) const;
    [[nodiscard]] PieceSet& pieces() { return m_pieces; }
    [[nodiscard]] const BoardTheme& theme() const noexcept { return m_theme; }
    [[nodiscard]] QSize sizeHint() const override { return {440, 440}; }

signals:
    /// The pieces changed (a click, a drag or a removal).
    void edited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRect boardRect() const;
    [[nodiscard]] std::optional<core::Square> squareAt(QPoint point) const;
    [[nodiscard]] QRect squareRect(core::Square square) const;

    BoardTheme m_theme;
    PieceSet m_pieces;
    core::BoardSetup m_setup;
    std::optional<core::Piece> m_tool;
    bool m_flipped = false;
    std::optional<core::Square> m_pressed; ///< where the mouse went down
    QPoint m_pressPoint;
    bool m_dragging = false;
    QPoint m_dragPoint;
    std::optional<core::Piece> m_carried; ///< from the palette
    QPoint m_carryPoint;
};

/// Setting up a position to start a game from: the board, a palette of pieces, the side to
/// move, castling, en passant and the move number, and the FEN, which can be typed or pasted.
/// What is still wrong with the position is said under it; "Start From Here" waits until it
/// can be played.
class PositionEditor : public QDialog {
    Q_OBJECT

public:
    /// Starts with `position`, drawn with the board's colours and pieces, from the side shown.
    PositionEditor(const AppStyle& style, const core::Position& position, const BoardTheme& theme,
                   const QString& pieceSet, bool flipped, QWidget* parent = nullptr);

    /// The position set up (only once it can be played).
    [[nodiscard]] std::optional<core::Position> position() const;
    [[nodiscard]] SetupBoard* board() const { return m_board; }

protected:
    /// Palette pieces dragged onto the board.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// One side's pieces (and a tool), for above or under the board.
    QWidget* buildPalette(const QString& pieceSet, core::Color color);
    QWidget* buildSettings();
    /// Shows the setup everywhere but the board (which has it already).
    void showSetup();
    void setSetup(const core::BoardSetup& setup);
    /// The board flipped: each side's palette goes to its side of the board.
    void swapPalettes();
    void restyleTools();

    const AppStyle& m_style;
    SetupBoard* m_board = nullptr;
    QBoxLayout* m_boardColumn = nullptr; ///< palette, board, palette
    QWidget* m_topPalette = nullptr;
    QWidget* m_bottomPalette = nullptr;
    QPushButton* m_moveTool = nullptr;
    QPushButton* m_removeTool = nullptr;
    QButtonGroup* m_tools = nullptr;
    QButtonGroup* m_side = nullptr;
    std::array<QPushButton*, 4> m_castle{}; ///< White O-O, White O-O-O, Black O-O, Black O-O-O
    QComboBox* m_enPassant = nullptr;
    Stepper* m_moveNumber = nullptr;
    QLineEdit* m_fen = nullptr;
    QLabel* m_problems = nullptr;
    QPushButton* m_start = nullptr;
    bool m_showing = false;             ///< showSetup() is filling the fields: their signals wait
    QPushButton* m_carryFrom = nullptr; ///< the palette piece pressed
    QPoint m_carryStart;                ///< where, on the screen
    bool m_carrying = false;            ///< it was dragged far enough to be carried
};

} // namespace kiebitz::app
