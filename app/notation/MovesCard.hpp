#pragma once

#include "kiebitz/core/game.hpp"

#include <QFrame>

#include <cstdint>
#include <utility>
#include <vector>

class QButtonGroup;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace kiebitz::app {

class AppStyle;
class NotationView;

/// The card with a game's moves: the title (with the opening), the move list (as text or rows),
/// a bar of symbols (!, ?, ±, ...) for the current move, its comment, the continuations at a
/// branch point, and the buttons to step through the game.
class MovesCard : public QFrame {
    Q_OBJECT

public:
    explicit MovesCard(AppStyle& style, QWidget* parent = nullptr);

    [[nodiscard]] NotationView* notation() const { return m_notation; }
    [[nodiscard]] QPlainTextEdit* commentBox() const { return m_commentBox; }

    /// "MOVES · Sicilian Defense: Najdorf Variation".
    void setTitle(const QString& title);
    /// Moves as rows (with comments and variations between) or as flowing text.
    void setRows(bool rows);
    /// The comment box shows the comment on `id`, under a title naming the move; the symbol
    /// bar, the symbols it has.
    void showComment(const core::Game& game, core::NodeId id);
    /// The symbol bar ticks the symbols `id` has (also done by showComment()).
    void showSymbols(const core::Game& game, core::NodeId id);
    /// The continuations after `at` when there are several (else hidden); `choice` is the one
    /// → would play. `hint`: the one-time "↑ ↓ to choose" line.
    void showContinuations(const core::Game* game, core::NodeId at, int choice, bool hint);
    void setNavigation(bool atStart, bool atEnd);
    /// The buttons' icons in the current colours.
    void restyle();
    /// A preview of a database game: a bar over the moves to merge them into `target` (a
    /// file's name) or close the preview. Empty: no bar.
    void setMergeTarget(const QString& target);

signals:
    void startRequested();
    void backRequested();
    void forwardRequested();
    void endRequested();
    void commentEdited(const QString& text);
    void continuationClicked(core::NodeId id);
    /// A symbol of the bar clicked: on the current move, it goes on or comes off.
    void symbolClicked(std::uint8_t nag);
    /// The preview's "Merge into ...": its moves up to the current one, into that file.
    void mergeRequested();
    void previewCloseRequested();
    /// "Set Up…": the position editor, from the position shown.
    void setUpRequested();

private:
    AppStyle& m_style;
    QLabel* m_title = nullptr;
    QWidget* m_mergeBar = nullptr;
    QPushButton* m_merge = nullptr;
    QButtonGroup* m_views = nullptr; ///< Text | Rows over the move list
    NotationView* m_notation = nullptr;
    std::vector<std::pair<std::uint8_t, QPushButton*>> m_symbols; ///< the bar's, by NAG
    QLabel* m_commentTitle = nullptr;
    QPlainTextEdit* m_commentBox = nullptr;
    QScrollArea* m_forkScroll = nullptr; ///< the continuations, under the moves
    QWidget* m_forkPanel = nullptr;      ///< in m_forkScroll
    QVBoxLayout* m_forkLayout = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_backButton = nullptr;
    QPushButton* m_forwardButton = nullptr;
    QPushButton* m_endButton = nullptr;
};

} // namespace kiebitz::app
