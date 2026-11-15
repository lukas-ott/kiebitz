#pragma once

#include "board/BoardTheme.hpp"
#include "kiebitz/core/game.hpp"
#include "kiebitz/repertoire/gaps.hpp"
#include "kiebitz/repertoire/prep.hpp"

#include <QWidget>

#include <memory>
#include <vector>

class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace kiebitz::app {

class AppStyle;
class BoardView;
class FlowLayout;
class ProfileStore;
class RepertoireStore;

/// A profile's "Prepare" tab: choosing what to play against the player. A board shows where
/// you are; the path above the moves leads back; the moves at that position are listed with
/// how their games went (White / draw / Black), how many and when last. Your repertoire's
/// moves are in the accent colour, and also the ones they never met are there. A click on a
/// move steps into it; its + puts the line into "Your prep", and "Create prep file" makes a
/// repertoire file of those lines, with their replies.
class PreparePage : public QWidget {
    Q_OBJECT

public:
    PreparePage(ProfileStore& store, RepertoireStore& repertoires, const AppStyle& style,
                QWidget* parent = nullptr);
    ~PreparePage() override;
    PreparePage(const PreparePage&) = delete;
    PreparePage& operator=(const PreparePage&) = delete;
    PreparePage(PreparePage&&) = delete;
    PreparePage& operator=(PreparePage&&) = delete;

    /// Shows the preparation against a profile's games (`part`: "" all, an account, "otb", a
    /// database's): worked out again only if they (or, with `force`, your repertoire) changed.
    void show(const QString& profileId, const QString& part, bool force = false);
    /// Your colour against them.
    void setYourColour(core::Color you);
    /// The board's colours and pieces, as on the Board page.
    void setBoardLook(const BoardTheme& theme, const QString& pieces);

    /// Where you are: the moves from the start.
    [[nodiscard]] const std::vector<core::Move>& line() const noexcept { return m_line; }
    /// Goes to the position after `line` (a line of their games, or of your repertoire). The
    /// moves after it on the last line chosen stay, so the arrows can step forward again.
    void stepTo(std::vector<core::Move> line);
    /// Steps `plies` back (negative) or forward along the line chosen, within its ends.
    void step(int plies);
    /// The lines in "Your prep".
    [[nodiscard]] const std::vector<std::vector<core::Move>>& prepLines() const noexcept {
        return m_lines;
    }

protected:
    void keyPressEvent(QKeyEvent* event) override;

signals:
    /// "Open on the board": a new game with the moves so far.
    void openLineRequested(const kiebitz::core::Game& game);
    /// "Create prep file": a repertoire file to save (`name`: "Petra Pawn – White").
    void prepFileRequested(const QString& name, const kiebitz::core::Game& game);
    /// The moves are shown (tests).
    void ready();

private:
    struct Analysis;
    void build();
    void showLine();
    void showPath();
    void showMoves();
    void showPrep();
    void toggleLine(const std::vector<core::Move>& line);
    void createPrepFile();

    ProfileStore& m_store;
    RepertoireStore& m_repertoires;
    const AppStyle& m_style;
    BoardView* m_board = nullptr;
    QPushButton* m_white = nullptr;
    QPushButton* m_black = nullptr;
    QHBoxLayout* m_path = nullptr;
    std::vector<QPushButton*> m_steps; ///< start, back, forward, end
    QLabel* m_summary = nullptr;
    QLabel* m_whose = nullptr; ///< "Your moves" / "Their replies"
    QTreeWidget* m_moves = nullptr;
    QWidget* m_prep = nullptr;
    FlowLayout* m_chips = nullptr;
    QPushButton* m_create = nullptr;
    QString m_profile;
    QString m_part;
    QString m_stamp;   ///< what the analysis shown was worked out from
    int m_request = 0; ///< newer requests win
    core::Color m_you = core::Color::White;
    std::shared_ptr<const Analysis> m_analysis;
    std::unique_ptr<repertoire::PrepTree> m_tree; ///< the player's games against your colour
    std::vector<core::Move> m_line;               ///< where you are
    std::vector<core::Move> m_ahead; ///< the line chosen last: `m_line` is the start of it
    std::vector<std::vector<core::Move>> m_lines; ///< "Your prep"
};

} // namespace kiebitz::app
