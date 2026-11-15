#include "PreparePage.hpp"

#include "ProfileStore.hpp"
#include "board/BoardView.hpp"
#include "repertoire/RepertoireStore.hpp"
#include "style/AppStyle.hpp"
#include "style/Icons.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/ResultBar.hpp"

#include <QButtonGroup>
#include <QDate>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

namespace kiebitz::app {
namespace {

constexpr int kMoveRole = Qt::UserRole; ///< a row: its move (UCI)

// Plain ints: they are QTreeWidget column numbers.
constexpr int kMove = 0;
constexpr int kResults = 1;
constexpr int kGames = 2;
constexpr int kLast = 3;
constexpr int kAdd = 4;

constexpr int kBoardSize = 260;

QLabel* label(const QString& text, const char* role) {
    auto* result = new QLabel(text);
    result->setProperty("role", role);
    return result;
}

/// "7. Nf3", "7… h6": a move after `before`, with its number.
QString moveText(std::span<const core::Move> before, core::Move move) {
    core::Position position;
    for (const core::Move played : before) {
        position.play(played);
    }
    return QStringLiteral("%1%2 %3")
        .arg(position.fullmoveNumber())
        .arg(position.sideToMove() == core::Color::White ? QStringLiteral(".")
                                                         : QStringLiteral("…"))
        .arg(QString::fromStdString(position.san(move)));
}

/// "Sep 2026" from yyyymmdd (a year alone: "2026"), or empty.
QString monthText(int date) {
    const int year = date / 10000;
    const int month = (date / 100) % 100;
    if (year == 0) {
        return {};
    }
    if (month == 0) {
        return QString::number(year);
    }
    return QLocale().toString(QDate(year, month, 1), QStringLiteral("MMM yyyy"));
}

} // namespace

/// The player's games (read in the background).
struct PreparePage::Analysis {
    std::vector<repertoire::PlayedGame> games;
    QString name; ///< the player, for the prep file
};

PreparePage::PreparePage(ProfileStore& store, RepertoireStore& repertoires, const AppStyle& style,
                         QWidget* parent)
    : QWidget(parent), m_store(store), m_repertoires(repertoires), m_style(style) {
    auto* box = new QHBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(20);

    // Left: the board where you are, and your colour.
    auto* left = new QVBoxLayout;
    left->setSpacing(10);
    m_board = new BoardView;
    m_board->setObjectName(QStringLiteral("prepareBoard"));
    m_board->setInteractive(false);
    m_board->setShowCoordinates(false);
    m_board->setFixedSize(kBoardSize, kBoardSize);
    left->addWidget(m_board);
    // Arrows (and ← → Home End) step along the moves chosen.
    auto* stepRow = new QHBoxLayout;
    stepRow->setSpacing(6);
    for (const auto& [tip, plies, name] : {std::tuple{tr("Start (Home)"), -10000, "prepareStart"},
                                           std::tuple{tr("Back (←)"), -1, "prepareBack"},
                                           std::tuple{tr("Forward (→)"), 1, "prepareForward"},
                                           std::tuple{tr("End (End)"), 10000, "prepareEnd"}}) {
        auto* button = new QPushButton;
        button->setObjectName(QString::fromLatin1(name));
        button->setProperty("role", "nav");
        button->setToolTip(tip);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(14, 14));
        connect(button, &QPushButton::clicked, this, [this, by = plies] {
            setFocus(); // from now on the arrow keys step too
            step(by);
        });
        stepRow->addWidget(button);
        m_steps.push_back(button);
    }
    stepRow->addStretch();
    left->addLayout(stepRow);
    auto* colourRow = new QHBoxLayout;
    colourRow->setSpacing(8);
    colourRow->addWidget(label(tr("I play"), "emptyHint"));
    auto* colours = new QWidget;
    colours->setProperty("role", "segmented");
    colours->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* coloursBox = new QHBoxLayout(colours);
    coloursBox->setContentsMargins(2, 2, 2, 2);
    coloursBox->setSpacing(2);
    auto* group = new QButtonGroup(this);
    for (const auto& [colour, text, name] :
         {std::tuple{core::Color::White, tr("White"), "prepareWhite"},
          std::tuple{core::Color::Black, tr("Black"), "prepareBlack"}}) {
        auto* button = new QPushButton(text);
        button->setObjectName(QString::fromLatin1(name));
        button->setProperty("role", "segment");
        button->setCheckable(true);
        button->setChecked(colour == m_you);
        button->setFocusPolicy(Qt::NoFocus);
        group->addButton(button);
        coloursBox->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, which = colour] { setYourColour(which); });
        (colour == core::Color::White ? m_white : m_black) = button;
    }
    group->setExclusive(true);
    colourRow->addWidget(colours);
    colourRow->addStretch();
    left->addLayout(colourRow);
    m_summary = label(QString(), "emptyHint");
    m_summary->setObjectName(QStringLiteral("prepareSummary"));
    left->addWidget(m_summary);
    left->addStretch();
    box->addLayout(left);

    // Right: the way here, the moves at this position, your prep.
    auto* right = new QVBoxLayout;
    right->setSpacing(8);
    auto* pathRow = new QHBoxLayout;
    pathRow->setSpacing(0);
    m_path = new QHBoxLayout;
    m_path->setSpacing(4);
    pathRow->addLayout(m_path);
    pathRow->addStretch();
    auto* open = new QPushButton(tr("Open on the board"));
    open->setObjectName(QStringLiteral("prepareOpen"));
    open->setProperty("role", "link");
    open->setFocusPolicy(Qt::NoFocus);
    connect(open, &QPushButton::clicked, this, [this] {
        core::Game game;
        core::NodeId node = core::Game::root();
        for (const core::Move move : m_line) {
            node = game.addMove(node, move);
        }
        emit openLineRequested(game);
    });
    pathRow->addWidget(open);
    right->addLayout(pathRow);
    m_whose = label(QString(), "cardTitle");
    m_whose->setObjectName(QStringLiteral("prepareWhose"));
    right->addWidget(m_whose);

    m_moves = new QTreeWidget;
    m_moves->setObjectName(QStringLiteral("prepareMoves"));
    m_moves->setColumnCount(5);
    m_moves->setHeaderLabels({tr("MOVE"), tr("RESULTS"), tr("GAMES"), tr("LAST"), QString()});
    m_moves->setRootIsDecorated(false);
    m_moves->setUniformRowHeights(true);
    m_moves->setFrameShape(QFrame::NoFrame);
    m_moves->setFocusPolicy(Qt::NoFocus);
    m_moves->header()->setStretchLastSection(false);
    m_moves->header()->setSectionResizeMode(kMove, QHeaderView::ResizeToContents);
    m_moves->header()->setSectionResizeMode(kResults, QHeaderView::Stretch);
    m_moves->header()->setSectionResizeMode(kGames, QHeaderView::ResizeToContents);
    m_moves->header()->setSectionResizeMode(kLast, QHeaderView::ResizeToContents);
    m_moves->header()->setSectionResizeMode(kAdd, QHeaderView::Fixed);
    m_moves->header()->resizeSection(kAdd, 32);
    m_moves->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    connect(m_moves, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == kAdd) {
            return; // its button
        }
        if (const auto move =
                core::Move::fromUci(item->data(kMove, kMoveRole).toString().toStdString())) {
            std::vector<core::Move> next = m_line;
            next.push_back(*move);
            stepTo(std::move(next));
        }
    });
    right->addWidget(m_moves, 1);

    // Your prep: the lines chosen, then the file.
    m_prep = new QWidget;
    m_prep->setObjectName(QStringLiteral("preparePrep"));
    auto* prepBox = new QVBoxLayout(m_prep);
    prepBox->setContentsMargins(0, 8, 0, 0);
    prepBox->setSpacing(8);
    auto* prepHead = new QHBoxLayout;
    prepHead->addWidget(label(tr("YOUR PREP"), "cardTitle"));
    prepHead->addStretch();
    m_create = new QPushButton(tr("Create Prep File"));
    m_create->setObjectName(QStringLiteral("prepareCreate"));
    m_create->setProperty("role", "primary");
    connect(m_create, &QPushButton::clicked, this, &PreparePage::createPrepFile);
    prepHead->addWidget(m_create);
    prepBox->addLayout(prepHead);
    auto* chips = new QWidget;
    chips->setObjectName(QStringLiteral("prepareChips"));
    m_chips = new FlowLayout(chips, 6);
    prepBox->addWidget(chips);
    right->addWidget(m_prep);
    box->addLayout(right, 1);
    setFocusPolicy(Qt::ClickFocus); // a click on the page takes the arrow keys
    showPrep();
    showLine();
}

PreparePage::~PreparePage() = default;

void PreparePage::setBoardLook(const BoardTheme& theme, const QString& pieces) {
    m_board->setTheme(theme);
    m_board->setPieceSet(pieces);
}

void PreparePage::setYourColour(core::Color you) {
    m_white->setChecked(you == core::Color::White);
    m_black->setChecked(you == core::Color::Black);
    if (you == m_you) {
        return;
    }
    m_you = you;
    m_line.clear(); // the other colour: other games, another prep
    m_ahead.clear();
    m_lines.clear();
    build();
}

void PreparePage::show(const QString& profileId, const QString& part, bool force) {
    const QString stamp = profileId + u'|' + part + u'|' + m_store.databaseStamp(profileId, part);
    if (!force && stamp == m_stamp && m_analysis) {
        return; // the same games: what is shown stands
    }
    if (m_profile != profileId) {
        // Another player: nothing of the last one stays.
        m_analysis.reset();
        m_tree.reset();
        m_line.clear();
        m_ahead.clear();
        m_lines.clear();
        m_moves->clear();
        showPrep();
    }
    m_stamp = stamp;
    m_profile = profileId;
    m_part = part;
    const int request = ++m_request;
    const Profile* profile = m_store.profile(profileId);
    const std::shared_ptr<const gamedb::Database> db = m_store.database(profileId, part);
    if (profile != nullptr && !db && ProfileStore::splitParts(part).size() > 1) {
        // Several sources, not combined yet: the profile page shows this again once they are.
        m_stamp.clear();
        m_summary->setText(tr("Reading their games…"));
        m_store.combine(profileId, part);
        return;
    }
    if (profile == nullptr || !db) {
        m_analysis.reset();
        m_tree.reset();
        m_summary->setText(tr("No games here yet."));
        showLine();
        emit ready();
        return;
    }
    if (!m_analysis) {
        m_summary->setText(tr("Reading their games…"));
    }
    QStringList names;
    for (const QString& name : ProfileStore::namesOf(*profile, part)) {
        names << name.toLower();
    }
    // Owned by the page and deleted when done (deleteLater); the analyzer can't see that.
    auto* watcher = new QFutureWatcher<std::shared_ptr<const Analysis>>(
        this); // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
    connect(watcher, &QFutureWatcher<std::shared_ptr<const Analysis>>::finished, this,
            [this, watcher, request] {
                watcher->deleteLater();
                if (request != m_request) {
                    return;
                }
                m_analysis = watcher->result();
                build();
            });
    watcher->setFuture(QtConcurrent::run([db, names, name = profile->name] {
        auto analysis = std::make_shared<Analysis>();
        analysis->name = name;
        const auto all = db->games(1, static_cast<std::size_t>(db->gameCount()));
        for (const gamedb::GameInfo& info : all) {
            const bool asWhite = names.contains(QString::fromStdString(info.white).toLower());
            const bool asBlack = names.contains(QString::fromStdString(info.black).toLower());
            if (asWhite == asBlack) {
                continue; // neither (or both): not theirs
            }
            const auto game = db->game(info.id);
            if (!game || game->startPosition().hash() != core::Position().hash()) {
                continue;
            }
            repertoire::PlayedGame one;
            one.side = asWhite ? core::Color::White : core::Color::Black;
            for (const core::NodeId node : game->mainLine()) {
                if (node != core::Game::root()) {
                    one.line.push_back(game->node(node).move);
                }
            }
            using gamedb::GameResult;
            if (info.result == GameResult::Draw) {
                one.points2 = 1;
            } else if (info.result == GameResult::WhiteWins) {
                one.points2 = asWhite ? 2 : 0;
            } else if (info.result == GameResult::BlackWins) {
                one.points2 = asWhite ? 0 : 2;
            }
            one.date = repertoire::dateNumber(info.date);
            analysis->games.push_back(std::move(one));
        }
        return std::shared_ptr<const Analysis>(std::move(analysis));
    }));
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)

void PreparePage::build() {
    if (!m_analysis) {
        m_tree.reset();
        showLine();
        return;
    }
    const BuiltRepertoire* yours = m_repertoires.get(m_you);
    m_tree = std::make_unique<repertoire::PrepTree>(m_analysis->games, m_you,
                                                    yours != nullptr ? &yours->tree : nullptr);
    const std::uint64_t games = m_tree->games();
    if (games == 0) {
        m_summary->setText(m_you == core::Color::White ? tr("No games of theirs with Black.")
                                                       : tr("No games of theirs with White."));
    } else {
        m_summary->setText(m_you == core::Color::White
                               ? tr("Their %1 games with Black").arg(QLocale().toString(games))
                               : tr("Their %1 games with White").arg(QLocale().toString(games)));
    }
    showLine();
    emit ready();
}

void PreparePage::stepTo(std::vector<core::Move> line) {
    const bool onTheWay =
        line.size() <= m_ahead.size() && std::equal(line.begin(), line.end(), m_ahead.begin());
    if (!onTheWay) {
        m_ahead = line; // another line: the moves ahead are gone
    }
    m_line = std::move(line);
    showLine();
}

void PreparePage::step(int plies) {
    const auto at = static_cast<std::ptrdiff_t>(m_line.size());
    const std::ptrdiff_t to =
        std::clamp<std::ptrdiff_t>(at + plies, 0, static_cast<std::ptrdiff_t>(m_ahead.size()));
    if (to != at) {
        stepTo(std::vector<core::Move>(m_ahead.begin(), m_ahead.begin() + to));
    }
}

void PreparePage::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Left:
        step(-1);
        break;
    case Qt::Key_Right:
        step(1);
        break;
    case Qt::Key_Home:
        step(-10000);
        break;
    case Qt::Key_End:
        step(10000);
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

void PreparePage::showLine() {
    core::Position position;
    for (const core::Move move : m_line) {
        position.play(move);
    }
    m_board->setFlipped(m_you == core::Color::Black);
    m_board->setPosition(position, m_line.empty() ? std::nullopt : std::optional(m_line.back()));
    const auto& c = m_style.colors();
    const std::array<IconKind, 4> kinds{IconKind::First, IconKind::Previous, IconKind::Next,
                                        IconKind::Last};
    const bool atStart = m_line.empty();
    const bool atEnd = m_line.size() == m_ahead.size();
    for (std::size_t i = 0; i < m_steps.size(); ++i) {
        m_steps[i]->setIcon(makeIcon(kinds[i], c.text, c.dimmed, 14));
        m_steps[i]->setEnabled(i < 2 ? !atStart : !atEnd);
    }
    showPath();
    showMoves();
}

void PreparePage::showPath() {
    // "Start › 1. e4 › 1… c5": each step a link to it, where you are marked; the moves after
    // it on the line chosen stay, to step forward again.
    while (const QLayoutItem* item = m_path->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto pathStep = [this](const QString& text, std::size_t plies, bool here) {
        auto* button = new QPushButton(text);
        button->setObjectName(QStringLiteral("preparePath:%1").arg(plies));
        button->setProperty("role", here ? "pathHere" : "link");
        button->setFocusPolicy(Qt::NoFocus);
        button->setEnabled(!here);
        connect(button, &QPushButton::clicked, this, [this, plies] {
            stepTo(std::vector<core::Move>(m_ahead.begin(),
                                           m_ahead.begin() + static_cast<std::ptrdiff_t>(plies)));
        });
        m_path->addWidget(button);
    };
    pathStep(tr("Start"), 0, m_line.empty());
    for (std::size_t ply = 0; ply < m_ahead.size(); ++ply) {
        m_path->addWidget(label(QStringLiteral("›"), "emptyHint"));
        pathStep(moveText(std::span(m_ahead).first(ply), m_ahead[ply]), ply + 1,
                 ply + 1 == m_line.size());
    }
}

void PreparePage::showMoves() {
    m_moves->clear();
    if (!m_tree) {
        m_whose->clear();
        return;
    }
    core::Position position;
    for (const core::Move move : m_line) {
        position.play(move);
    }
    const bool yours = position.sideToMove() == m_you;
    m_whose->setText(yours ? tr("YOUR MOVES") : tr("THEIR REPLIES"));
    const std::uint64_t here = m_tree->gamesAt(m_line);
    const auto& c = m_style.colors();
    const auto moves = m_tree->moves(m_line);
    for (const repertoire::PrepMove& move : moves) {
        auto* item = new QTreeWidgetItem(m_moves);
        item->setData(kMove, kMoveRole, QString::fromStdString(move.move.uci()));
        item->setText(kMove, moveText(m_line, move.move));
        QFont font = item->font(kMove);
        font.setWeight(QFont::DemiBold);
        item->setFont(kMove, font);
        if (move.yours && move.inRepertoire) {
            item->setForeground(kMove, c.accent);
            item->setToolTip(kMove, tr("Your repertoire's move"));
        }
        if (move.games > 0) {
            // How their games went, as White won / drawn / Black won (as in the Explorer).
            auto* cell = new QWidget;
            auto* cellBox = new QVBoxLayout(cell);
            cellBox->setContentsMargins(4, 0, 6, 0);
            auto* bar = new ResultBar(m_style, 14);
            const bool theyAreWhite = m_you == core::Color::Black;
            bar->setResults(theyAreWhite ? move.won : move.lost, move.drawn,
                            theyAreWhite ? move.lost : move.won);
            cellBox->addWidget(bar, 0, Qt::AlignVCenter);
            m_moves->setItemWidget(item, kResults, cell);
            item->setText(kGames, here > 0 ? QStringLiteral("%1 · %2%")
                                                 .arg(QLocale().toString(move.games))
                                                 .arg(((move.games * 100) + (here / 2)) / here)
                                           : QLocale().toString(move.games));
        } else {
            item->setText(kResults, tr("never met"));
        }
        item->setText(kLast, monthText(move.lastDate));
        for (const int column : {kResults, kGames, kLast}) {
            item->setForeground(column, c.muted);
        }
        // + puts the line into your prep (a tick: it is there; a click takes it out).
        std::vector<core::Move> line = m_line;
        line.push_back(move.move);
        const bool chosen = std::find(m_lines.begin(), m_lines.end(), line) != m_lines.end();
        auto* add = new QPushButton;
        add->setObjectName(QStringLiteral("prepareAdd:") + item->text(kMove));
        add->setProperty("role", "nav");
        add->setFocusPolicy(Qt::NoFocus);
        add->setFixedSize(24, 24);
        add->setIcon(makeIcon(chosen ? IconKind::Check : IconKind::Plus,
                              chosen ? c.accent : c.muted, c.dimmed, 14));
        add->setToolTip(chosen ? tr("In your prep: click to take it out")
                               : tr("Add this line to your prep"));
        connect(add, &QPushButton::clicked, this, [this, line] { toggleLine(line); });
        m_moves->setItemWidget(item, kAdd, add);
    }
    if (moves.empty()) {
        auto* item = new QTreeWidgetItem(m_moves);
        item->setText(kMove, tr("None of their games go further."));
        item->setForeground(kMove, c.muted);
        item->setFirstColumnSpanned(true);
        item->setFlags(Qt::NoItemFlags);
    }
}

void PreparePage::toggleLine(const std::vector<core::Move>& line) {
    const auto found = std::find(m_lines.begin(), m_lines.end(), line);
    if (found != m_lines.end()) {
        m_lines.erase(found);
    } else {
        // A longer line takes the place of the shorter one it continues.
        std::erase_if(m_lines, [&](const std::vector<core::Move>& other) {
            return other.size() < line.size() &&
                   std::equal(other.begin(), other.end(), line.begin());
        });
        m_lines.push_back(line);
    }
    showPrep();
    showMoves();
}

void PreparePage::showPrep() {
    while (const QLayoutItem* item = m_chips->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_prep->setVisible(!m_lines.empty());
    const auto& c = m_style.colors();
    for (const std::vector<core::Move>& line : m_lines) {
        // A chip: the line (a click goes there), and × to take it out.
        auto* chip = new QWidget;
        chip->setProperty("role", "prepChip");
        chip->setAttribute(Qt::WA_StyledBackground); // its rounded tint, from the style sheet
        auto* chipBox = new QHBoxLayout(chip);
        chipBox->setContentsMargins(10, 2, 4, 2);
        chipBox->setSpacing(2);
        auto* go = new QPushButton(QString::fromStdString(repertoire::lineText(line)));
        go->setProperty("role", "chipText");
        go->setFocusPolicy(Qt::NoFocus);
        // NOLINTNEXTLINE(bugprone-exception-escape): a slot, as any other
        connect(go, &QPushButton::clicked, this, [this, line] { stepTo(line); });
        auto* remove = new QPushButton;
        remove->setProperty("role", "chipText");
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setIcon(makeIcon(IconKind::Close, c.accent, c.dimmed, 10));
        remove->setToolTip(tr("Take it out"));
        // NOLINTNEXTLINE(bugprone-exception-escape): a slot, as any other
        connect(remove, &QPushButton::clicked, this, [this, line] { toggleLine(line); });
        chipBox->addWidget(go);
        chipBox->addWidget(remove);
        m_chips->addWidget(chip);
    }
}

void PreparePage::createPrepFile() {
    if (!m_tree || !m_analysis || m_lines.empty()) {
        return;
    }
    const QString name = m_analysis->name;
    repertoire::PrepTree::FileOptions options;
    options.event = tr("Preparation: %1").arg(name).toStdString();
    options.comment = [name](const repertoire::PrepMove& move) {
        QString text = move.games == 1 ? tr("%1: 1 game").arg(name)
                                       : tr("%1: %2 games").arg(name).arg(move.games);
        if (const double score = move.score(); score >= 0) {
            text += QStringLiteral(", ") + tr("scores %1%").arg(qRound(score * 100));
        }
        if (const QString last = monthText(move.lastDate); !last.isEmpty()) {
            text += QStringLiteral(", ") + tr("last %1").arg(last);
        }
        return text.toStdString();
    };
    const core::Game game = m_tree->prepFile(m_lines, options);
    emit prepFileRequested(QStringLiteral("%1 – %2").arg(
                               name, m_you == core::Color::White ? tr("White") : tr("Black")),
                           game);
}

} // namespace kiebitz::app
