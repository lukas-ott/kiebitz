#include "MovesCard.hpp"

#include "kiebitz/core/annotations.hpp"
#include "notation/NotationView.hpp"
#include "settings/Settings.hpp"
#include "style/AppStyle.hpp"
#include "style/Icons.hpp"
#include "style/Layout.hpp"
#include "widgets/Card.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>
#include <utility>

namespace kiebitz::app {
namespace {

/// The move list's own minimum: a few lines of moves, always.
constexpr int kNotationMin = 112;
/// The most the continuations take (the hint and three moves; more scroll). Their room is part
/// of the card's minimum height, so showing them never pushes the panels below.
constexpr int kContinuationsMax = 132;

/// "12… Nf6": a move as the move list numbers it.
QString numbered(const core::Game& game, core::NodeId id) {
    const bool white = game.mover(id) == core::Color::White;
    return QStringLiteral("%1%2 %3")
        .arg(game.moveNumber(id))
        .arg(white ? QStringLiteral(".") : QStringLiteral("…"))
        .arg(QString::fromStdString(game.node(id).san));
}

} // namespace

MovesCard::MovesCard(AppStyle& style, QWidget* parent) : QFrame(parent), m_style(style) {
    setProperty("card", true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(layout::kCard);
    layout->setSpacing(layout::kCardSpacing);
    m_title = ui::label(tr("MOVES"), "cardTitle");
    m_title->setObjectName(QStringLiteral("movesTitle"));
    // The title, and how to show the moves: flowing text or rows (remembered).
    auto* titleRow = new QHBoxLayout;
    titleRow->addWidget(m_title, 1);
    auto* views = new QWidget;
    views->setObjectName(QStringLiteral("notationViews"));
    views->setProperty("role", "segmented");
    auto* viewsLayout = new QHBoxLayout(views);
    viewsLayout->setContentsMargins(2, 2, 2, 2);
    viewsLayout->setSpacing(2);
    m_views = new QButtonGroup(views);
    const bool rows = Settings::instance().flag(Settings::kNotationRows);
    for (const auto& [id, text, tip] :
         {std::tuple{0, tr("Text"), tr("Moves as flowing text")},
          std::tuple{1, tr("Rows"), tr("Moves in rows, with comments and variations between")}}) {
        auto* segment = new QPushButton(text);
        segment->setObjectName(id == 0 ? QStringLiteral("notationText")
                                       : QStringLiteral("notationRows"));
        segment->setProperty("role", "segment");
        segment->setCheckable(true);
        segment->setChecked((id == 1) == rows);
        segment->setFocusPolicy(Qt::NoFocus);
        segment->setToolTip(tip);
        m_views->addButton(segment, id);
        viewsLayout->addWidget(segment);
    }
    titleRow->addWidget(views);
    layout->addLayout(titleRow);

    // A database game looked at next to a file: its moves (up to where you are) can go into
    // that file, or the preview closes.
    m_mergeBar = new QFrame;
    m_mergeBar->setObjectName(QStringLiteral("mergeBar"));
    m_mergeBar->setProperty("statTile", true);
    auto* mergeBox = new QHBoxLayout(m_mergeBar);
    mergeBox->setContentsMargins(12, 8, 8, 8);
    mergeBox->setSpacing(8);
    auto* mergeText = ui::label(tr("Preview"), "emptyHint");
    mergeText->setToolTip(tr("A game from a database. Step to the move you want, then merge: "
                             "its moves up to there go into the file, as a variation where "
                             "they differ."));
    mergeBox->addWidget(mergeText, 1);
    m_merge = new QPushButton;
    m_merge->setObjectName(QStringLiteral("mergeIntoFile"));
    m_merge->setProperty("role", "primary");
    m_merge->setFocusPolicy(Qt::NoFocus);
    connect(m_merge, &QPushButton::clicked, this, &MovesCard::mergeRequested);
    mergeBox->addWidget(m_merge);
    auto* closePreview = new QPushButton(tr("Close"));
    closePreview->setObjectName(QStringLiteral("closePreview"));
    closePreview->setProperty("role", "secondary");
    closePreview->setFocusPolicy(Qt::NoFocus);
    connect(closePreview, &QPushButton::clicked, this, &MovesCard::previewCloseRequested);
    mergeBox->addWidget(closePreview);
    m_mergeBar->hide();
    layout->addWidget(m_mergeBar);

    // The moves, and under them (at a branch point) the continuations to choose from (↑ ↓ to
    // choose, → to play). They share one area, so the continuations only take the move list's
    // room: the comment box, the buttons and the panels below stay where they are.
    auto* movesArea = new QWidget;
    movesArea->setObjectName(QStringLiteral("movesArea"));
    auto* areaLayout = new QVBoxLayout(movesArea);
    areaLayout->setContentsMargins(0, 0, 0, 0);
    areaLayout->setSpacing(layout::kCardSpacing);
    m_notation = new NotationView(m_style);
    m_notation->setMinimumHeight(kNotationMin);
    m_notation->setRows(rows);
    connect(m_views, &QButtonGroup::idClicked, this,
            [](int id) { Settings::instance().setValue(Settings::kNotationRows, id == 1); });
    areaLayout->addWidget(m_notation, 1);
    m_forkPanel = new QWidget;
    m_forkPanel->setObjectName(QStringLiteral("continuationsContent"));
    m_forkLayout = new QVBoxLayout(m_forkPanel);
    m_forkLayout->setContentsMargins(0, 0, 0, 0);
    m_forkLayout->setSpacing(4);
    m_forkPanel->setAutoFillBackground(false);
    m_forkScroll = new QScrollArea;
    m_forkScroll->setObjectName(QStringLiteral("continuations"));
    m_forkScroll->setWidget(m_forkPanel);
    m_forkScroll->setWidgetResizable(true);
    m_forkScroll->setFrameShape(QFrame::NoFrame);
    m_forkScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_forkScroll->viewport()->setAutoFillBackground(false);
    m_forkScroll->hide();
    areaLayout->addWidget(m_forkScroll);
    movesArea->setMinimumHeight(kNotationMin + layout::kCardSpacing + kContinuationsMax);
    layout->addWidget(movesArea, 1);

    // Symbols for the current move, a click each: how good it is, who stands better, and
    // a few more. Ticked: the move has it.
    auto* symbols = new QWidget;
    symbols->setObjectName(QStringLiteral("symbolBar"));
    auto* symbolsBox = new QHBoxLayout(symbols);
    symbolsBox->setContentsMargins(0, 0, 0, 0);
    symbolsBox->setSpacing(1);
    const auto symbolGroup = [&](std::initializer_list<std::pair<std::uint8_t, QString>> group) {
        for (const auto& [nag, tip] : group) {
            const auto text = core::nagSymbol(nag);
            auto* button = new QPushButton(
                QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
            button->setObjectName(QStringLiteral("symbol:%1").arg(nag));
            button->setProperty("role", "symbol");
            button->setCheckable(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setToolTip(tip);
            connect(button, &QPushButton::clicked, this,
                    [this, which = nag] { emit symbolClicked(which); });
            symbolsBox->addWidget(button);
            m_symbols.emplace_back(nag, button);
        }
        symbolsBox->addSpacing(8);
    };
    symbolGroup({{3, tr("Brilliant move")},
                 {1, tr("Good move")},
                 {5, tr("Interesting move")},
                 {6, tr("Dubious move")},
                 {2, tr("Mistake")},
                 {4, tr("Blunder")}});
    symbolGroup({{18, tr("White is winning")},
                 {16, tr("White is better")},
                 {14, tr("White is slightly better")},
                 {10, tr("Equal")},
                 {15, tr("Black is slightly better")},
                 {17, tr("Black is better")},
                 {19, tr("Black is winning")},
                 {13, tr("Unclear")}});
    symbolGroup({{7, tr("Only move")}, {146, tr("Novelty")}});
    symbolsBox->addStretch();
    layout->addWidget(symbols);

    // The current move's comment, edited in place, under a header naming the move.
    m_commentTitle = ui::label(QString(), "cardTitle");
    layout->addWidget(m_commentTitle);
    m_commentBox = new QPlainTextEdit;
    m_commentBox->setObjectName(QStringLiteral("commentBox"));
    m_commentBox->setTabChangesFocus(true);
    m_commentBox->setFixedHeight(58);
    connect(m_commentBox, &QPlainTextEdit::textChanged, this,
            [this] { emit commentEdited(m_commentBox->toPlainText()); });
    layout->addWidget(m_commentBox);

    // Start / back / forward / end; each mirrors a Game menu action. Icons: restyle().
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    const auto addButton = [&](const QString& tip, void (MovesCard::*signal)()) {
        auto* button = new QPushButton;
        button->setIconSize(QSize(16, 16));
        button->setProperty("role", "nav");
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, signal);
        buttons->addWidget(button);
        return button;
    };
    m_startButton = addButton(tr("Start (Home)"), &MovesCard::startRequested);
    m_backButton = addButton(tr("Back (←)"), &MovesCard::backRequested);
    m_forwardButton = addButton(tr("Forward (→)"), &MovesCard::forwardRequested);
    m_endButton = addButton(tr("End (End)"), &MovesCard::endRequested);
    // Setting up a position, starting from the one on the board (also Game → Set Up Position…).
    buttons->addSpacing(6);
    auto* setUp = new QPushButton(tr("Set Up…"));
    setUp->setObjectName(QStringLiteral("setUpButton"));
    setUp->setProperty("role", "secondary");
    setUp->setToolTip(tr("Set up a position on a board, starting from this one"));
    setUp->setFocusPolicy(Qt::NoFocus);
    connect(setUp, &QPushButton::clicked, this, &MovesCard::setUpRequested);
    buttons->addWidget(setUp);
    layout->addLayout(buttons);
    restyle();
}

void MovesCard::setTitle(const QString& title) {
    m_title->setText(title);
}

void MovesCard::setRows(bool rows) {
    m_notation->setRows(rows);
    if (auto* button = m_views->button(rows ? 1 : 0)) {
        button->setChecked(true);
    }
}

void MovesCard::setMergeTarget(const QString& target) {
    m_mergeBar->setVisible(!target.isEmpty());
    m_merge->setText(tr("Merge into %1").arg(target));
}

void MovesCard::showSymbols(const core::Game& game, core::NodeId id) {
    // The symbols it has; the game's start has none.
    const auto& nags = game.node(id).nags;
    for (const auto& [nag, button] : m_symbols) {
        button->setChecked(std::find(nags.begin(), nags.end(), nag) != nags.end());
        button->setEnabled(id != core::Game::root());
    }
}

void MovesCard::showComment(const core::Game& game, core::NodeId id) {
    {
        const QSignalBlocker block(m_commentBox);
        // The words only: [%clk ...] and other tools' commands stay in the game, unseen.
        m_commentBox->setPlainText(
            QString::fromStdString(core::commentText(game.node(id).comment)));
        m_commentBox->setPlaceholderText(id == core::Game::root()
                                             ? tr("Add a comment about the game…")
                                             : tr("Add a comment about this move…"));
    }
    showSymbols(game, id);
    // "COMMENT · 12… Nf6": which move the box belongs to.
    const QString move = id == core::Game::root() ? tr("GAME") : numbered(game, id);
    m_commentTitle->setText(tr("COMMENT") + QStringLiteral("  ·  ") + move);
}

void MovesCard::showContinuations(const core::Game* game, core::NodeId at, int choice, bool hint) {
    // Rebuild the list (it's at most a handful of buttons).
    while (const auto* item = m_forkLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto children = game != nullptr ? game->children(at) : std::vector<core::NodeId>{};
    if (game == nullptr || children.size() < 2) {
        m_forkScroll->hide();
        return;
    }
    if (hint) {
        m_forkLayout->addWidget(
            ui::label(tr("↑ ↓ to choose a continuation, → to play it"), "hint"));
    }
    QPushButton* chosen = nullptr;
    for (std::size_t i = 0; i < children.size(); ++i) {
        const core::NodeId child = children[i];
        auto* button = new QPushButton(numbered(*game, child));
        button->setProperty("role", "fork");
        button->setProperty("chosen", std::cmp_equal(i, choice));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this,
                [this, child] { emit continuationClicked(child); });
        m_forkLayout->addWidget(button);
        if (std::cmp_equal(i, choice)) {
            chosen = button;
        }
    }
    // As tall as the moves need, up to the room kept for them; the rest scrolls.
    m_forkScroll->setFixedHeight(std::min(m_forkLayout->sizeHint().height(), kContinuationsMax));
    m_forkScroll->show();
    if (chosen != nullptr) {
        m_forkScroll->ensureWidgetVisible(chosen, 0, 0);
    }
}

void MovesCard::setNavigation(bool atStart, bool atEnd) {
    m_backButton->setEnabled(!atStart);
    m_startButton->setEnabled(!atStart);
    m_forwardButton->setEnabled(!atEnd);
    m_endButton->setEnabled(!atEnd);
}

void MovesCard::restyle() {
    const auto& c = m_style.colors();
    m_startButton->setIcon(makeIcon(IconKind::First, c.text, c.dimmed));
    m_backButton->setIcon(makeIcon(IconKind::Previous, c.text, c.dimmed));
    m_forwardButton->setIcon(makeIcon(IconKind::Next, c.text, c.dimmed));
    m_endButton->setIcon(makeIcon(IconKind::Last, c.text, c.dimmed));
}

} // namespace kiebitz::app
