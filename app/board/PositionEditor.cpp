#include "PositionEditor.hpp"

#include "style/AppStyle.hpp"
#include "style/Icons.hpp"
#include "widgets/Stepper.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace kiebitz::app {

using core::BoardSetup;
using core::Color;
using core::Piece;
using core::PieceType;
using core::Square;

// ---- The board ---------------------------------------------------------------------------------

SetupBoard::SetupBoard(BoardTheme theme, const QString& pieceSet, QWidget* parent)
    : QWidget(parent), m_theme(std::move(theme)), m_pieces(pieceSet) {
    setObjectName(QStringLiteral("setupBoard"));
    setMinimumSize(320, 320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);
}

void SetupBoard::setSetup(const BoardSetup& setup) {
    m_setup = setup;
    update();
}

void SetupBoard::setFlipped(bool flipped) {
    m_flipped = flipped;
    update();
}

QRect SetupBoard::boardRect() const {
    const int side = (std::min(width(), height()) / 8) * 8;
    return {(width() - side) / 2, (height() - side) / 2, side, side};
}

QRect SetupBoard::squareRect(Square square) const {
    const QRect board = boardRect();
    const int size = board.width() / 8;
    const int column = m_flipped ? 7 - square.file() : square.file();
    const int row = m_flipped ? square.rank() : 7 - square.rank();
    return {board.left() + (column * size), board.top() + (row * size), size, size};
}

std::optional<Square> SetupBoard::squareAt(QPoint point) const {
    const QRect board = boardRect();
    if (!board.contains(point) || board.width() < 8) {
        return std::nullopt;
    }
    const int size = board.width() / 8;
    const int column = std::clamp((point.x() - board.left()) / size, 0, 7);
    const int row = std::clamp((point.y() - board.top()) / size, 0, 7);
    return m_flipped ? Square::at(7 - column, row) : Square::at(column, 7 - row);
}

QPoint SetupBoard::squareCenter(Square square) const {
    return squareRect(square).center();
}

void SetupBoard::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const int size = boardRect().width() / 8;
    const qreal ratio = devicePixelRatioF();
    QFont font = painter.font();
    font.setPixelSize(std::max(9, size / 6));
    font.setBold(true);
    painter.setFont(font);
    const int pad = std::max(2, size / 20);
    for (int index = 0; index < 64; ++index) {
        const Square square = Square::fromIndex(index);
        const QRect rect = squareRect(square);
        const bool light = (square.file() + square.rank()) % 2 == 1;
        painter.fillRect(rect, light ? m_theme.light : m_theme.dark);
        // Coordinates along the edge, as on the main board.
        painter.setPen(light ? m_theme.dark : m_theme.light);
        const int bottomRank = m_flipped ? 7 : 0;
        const int leftFile = m_flipped ? 7 : 0;
        if (square.rank() == bottomRank) {
            painter.drawText(rect.adjusted(pad, pad, -pad, -pad),
                             static_cast<int>(Qt::AlignRight | Qt::AlignBottom),
                             QString(QChar('a' + square.file())));
        }
        if (square.file() == leftFile) {
            painter.drawText(rect.adjusted(pad, pad, -pad, -pad),
                             static_cast<int>(Qt::AlignLeft | Qt::AlignTop),
                             QString::number(square.rank() + 1));
        }
        const Piece piece = m_setup.at(square);
        if (piece.isNone()) {
            continue;
        }
        painter.setOpacity(m_dragging && m_pressed == square ? 0.3 : 1.0);
        painter.drawPixmap(rect.topLeft(), m_pieces.pixmap(piece, size, ratio));
        painter.setOpacity(1.0);
    }
    if (m_dragging && m_pressed) {
        const QPoint at = m_dragPoint - QPoint(size / 2, size / 2);
        painter.drawPixmap(at, m_pieces.pixmap(m_setup.at(*m_pressed), size, ratio));
    }
}

void SetupBoard::mousePressEvent(QMouseEvent* event) {
    const auto square = squareAt(event->position().toPoint());
    if (!square) {
        return;
    }
    if (event->button() == Qt::RightButton) {
        if (!m_setup.at(*square).isNone()) {
            m_setup.put(*square, {});
            update();
            emit edited();
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_pressed = square;
    m_pressPoint = event->position().toPoint();
    m_dragging = false;
}

void SetupBoard::mouseMoveEvent(QMouseEvent* event) {
    if (!m_pressed || m_setup.at(*m_pressed).isNone()) {
        return;
    }
    m_dragPoint = event->position().toPoint();
    if (!m_dragging &&
        (m_dragPoint - m_pressPoint).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragging = true;
    }
    if (m_dragging) {
        update();
    }
}

void SetupBoard::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const std::optional<Square> pressed = std::exchange(m_pressed, std::nullopt);
    if (!pressed) {
        return;
    }
    const Square from = *pressed;
    if (std::exchange(m_dragging, false)) {
        // Dragged: to another square, or off the board (removed).
        const Piece piece = m_setup.at(from);
        m_setup.put(from, {});
        if (const auto to = squareAt(event->position().toPoint())) {
            m_setup.put(*to, piece);
        }
        update();
        emit edited();
        return;
    }
    if (!m_tool) {
        return; // no piece chosen: a click only picks pieces up to drag
    }
    // A click: the chosen piece goes there, or comes off if it is already there.
    const Piece now = m_setup.at(from);
    m_setup.put(from, now == *m_tool ? Piece{} : *m_tool);
    update();
    emit edited();
}

// ---- The dialog --------------------------------------------------------------------------------

namespace {

constexpr int kSquare = 56; ///< the board's squares
constexpr int kTool = 56;   ///< a palette button
constexpr int kSideWidth = 248;

QPushButton* segment(const QString& text, const QString& name) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setProperty("role", "segment");
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QPushButton* chip(const QString& text, const QString& name) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setProperty("role", "chip");
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QPushButton* action(const QString& text, const QString& name) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setProperty("role", "secondary");
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QLabel* caption(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("role", "cardTitle");
    return label;
}

constexpr std::array kTypes{PieceType::King,   PieceType::Queen,  PieceType::Rook,
                            PieceType::Bishop, PieceType::Knight, PieceType::Pawn};

/// Tool ids in the palette's button group: 0-5 White's pieces, 6-11 Black's, then these.
constexpr int kMoveTool = 12;
constexpr int kRemoveTool = 13;

} // namespace

PositionEditor::PositionEditor(const AppStyle& style, const core::Position& position,
                               const BoardTheme& theme, const QString& pieceSet, bool flipped,
                               QWidget* parent)
    : QDialog(parent), m_style(style) {
    setObjectName(QStringLiteral("positionEditor"));
    setWindowTitle(tr("Set Up Position"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    // The board between the two palettes: the side at the top's pieces above it.
    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    auto* boardColumn = new QVBoxLayout;
    m_boardColumn = boardColumn;
    boardColumn->setSpacing(10);
    m_board = new SetupBoard(theme, pieceSet);
    m_board->setFixedSize(8 * kSquare, 8 * kSquare);
    m_board->setFlipped(flipped);
    m_tools = new QButtonGroup(this);
    m_tools->setExclusive(true);
    m_topPalette = buildPalette(pieceSet, flipped ? Color::White : Color::Black);
    m_bottomPalette = buildPalette(pieceSet, flipped ? Color::Black : Color::White);
    boardColumn->addWidget(m_topPalette, 0, Qt::AlignHCenter);
    boardColumn->addWidget(m_board, 0, Qt::AlignHCenter);
    boardColumn->addWidget(m_bottomPalette, 0, Qt::AlignHCenter);
    auto* hint = new QLabel(tr("Click to put the chosen piece down · drag to move · drag off "
                               "the board or right-click to remove"));
    hint->setProperty("role", "emptyHint");
    hint->setAlignment(Qt::AlignHCenter);
    hint->setWordWrap(true);
    hint->setFixedWidth(8 * kSquare);
    boardColumn->addWidget(hint);
    body->addLayout(boardColumn);
    body->addWidget(buildSettings(), 0, Qt::AlignTop);
    layout->addLayout(body);

    // The FEN: shown as it changes, typed or pasted to set the whole board at once.
    auto* fenRow = new QHBoxLayout;
    fenRow->setSpacing(8);
    m_fen = new QLineEdit;
    m_fen->setObjectName(QStringLiteral("setupFen"));
    m_fen->setPlaceholderText(tr("FEN"));
    connect(m_fen, &QLineEdit::textEdited, this, [this](const QString& text) {
        auto parsed = core::Position::fromFen(text.trimmed().toStdString());
        if (parsed) {
            m_board->setSetup(BoardSetup::from(*parsed));
            showSetup();
        }
    });
    fenRow->addWidget(m_fen, 1);
    auto* copy = action(tr("Copy FEN"), QStringLiteral("setupCopyFen"));
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_fen->text()); });
    fenRow->addWidget(copy);
    layout->addLayout(fenRow);

    // What is still wrong, next to the buttons that wait for it.
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_problems = new QLabel;
    m_problems->setObjectName(QStringLiteral("setupProblems"));
    m_problems->setProperty("role", "error");
    m_problems->setWordWrap(true);
    buttons->addWidget(m_problems, 1);
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName(QStringLiteral("setupCancel"));
    cancel->setProperty("role", "secondary");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    m_start = new QPushButton(tr("Start From Here"));
    m_start->setObjectName(QStringLiteral("setupStart"));
    m_start->setProperty("role", "primary");
    m_start->setDefault(true);
    connect(m_start, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(m_start);
    layout->addLayout(buttons);

    connect(m_tools, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == kMoveTool) {
            m_board->setTool(std::nullopt);
        } else if (id == kRemoveTool) {
            m_board->setTool(Piece{});
        } else {
            m_board->setTool(Piece{kTypes[static_cast<std::size_t>(id % 6)],
                                   id < 6 ? Color::White : Color::Black});
        }
    });
    m_tools->button(kMoveTool)->setChecked(true);
    connect(m_board, &SetupBoard::edited, this, &PositionEditor::showSetup);
    setSetup(BoardSetup::from(position));
    m_board->setFocusPolicy(Qt::StrongFocus);
    m_board->setFocus(); // not the move number: its field would show a cursor
}

QWidget* PositionEditor::buildPalette(const QString& pieceSet, Color color) {
    // One side's six pieces, then a tool: White's row ends with Move (pick up and drag),
    // Black's with Remove. As wide as the board.
    auto* palette = new QWidget;
    palette->setObjectName(color == Color::White ? QStringLiteral("whitePalette")
                                                 : QStringLiteral("blackPalette"));
    palette->setProperty("role", "palette");
    palette->setAttribute(Qt::WA_StyledBackground);
    palette->setFixedWidth(8 * kSquare); // as wide as the board
    auto* row = new QHBoxLayout(palette);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(4);
    PieceSet pieces(pieceSet);
    const qreal ratio = devicePixelRatioF();
    const auto tool = [&](const QString& name, const QString& tip, int id) {
        auto* button = new QPushButton;
        button->setObjectName(name);
        button->setProperty("role", "pieceTool");
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        button->setFixedSize(kTool, kTool);
        m_tools->addButton(button, id);
        row->addWidget(button);
        return button;
    };
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        const Piece piece{kTypes[i], color};
        auto* button = tool(QStringLiteral("setupPiece:") + QChar(piece.fenChar()), QString(),
                            (color == Color::White ? 0 : 6) + static_cast<int>(i));
        // On a square of the board's light colour: both colours show on any app theme.
        const int side = kTool - 8;
        QPixmap tile(QSize(side, side) * ratio);
        tile.setDevicePixelRatio(ratio);
        tile.fill(Qt::transparent);
        {
            QPainter painter(&tile);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_board->theme().light);
            painter.drawRoundedRect(QRectF(0, 0, side, side), 5, 5);
            painter.drawPixmap(QPoint(3, 3), pieces.pixmap(piece, side - 6, ratio));
        }
        button->setIcon(QIcon(tile));
        button->setIconSize(QSize(side, side));
    }
    row->addStretch();
    if (color == Color::White) {
        m_moveTool = tool(QStringLiteral("setupMove"),
                          tr("Move: clicks only pick pieces up, to drag them"), kMoveTool);
    } else {
        m_removeTool =
            tool(QStringLiteral("setupRemove"), tr("Remove: clicks take pieces off"), kRemoveTool);
    }
    return palette;
}

QWidget* PositionEditor::buildSettings() {
    auto* card = new QFrame;
    card->setProperty("card", true);
    card->setFixedWidth(kSideWidth);
    auto* box = new QVBoxLayout(card);
    box->setContentsMargins(16, 16, 16, 16);
    box->setSpacing(8);

    box->addWidget(caption(tr("TO MOVE")));
    auto* white = segment(tr("White"), QStringLiteral("setupSide:w"));
    auto* black = segment(tr("Black"), QStringLiteral("setupSide:b"));
    m_side = new QButtonGroup(card);
    m_side->setExclusive(true);
    m_side->addButton(white, 0);
    m_side->addButton(black, 1);
    auto* sides = new QWidget;
    sides->setProperty("role", "segmented");
    auto* sidesRow = new QHBoxLayout(sides);
    sidesRow->setContentsMargins(2, 2, 2, 2);
    sidesRow->setSpacing(2);
    sidesRow->addWidget(white);
    sidesRow->addWidget(black);
    box->addWidget(sides);
    connect(m_side, &QButtonGroup::idClicked, this, [this](int id) {
        BoardSetup setup = m_board->setup();
        setup.sideToMove = id == 0 ? Color::White : Color::Black;
        m_board->setSetup(setup);
        showSetup();
    });
    box->addSpacing(10);

    // Castling: chips, only where king and rook are still at home.
    box->addWidget(caption(tr("CASTLING")));
    const std::array<QString, 4> names{QStringLiteral("K"), QStringLiteral("Q"),
                                       QStringLiteral("k"), QStringLiteral("q")};
    auto* castling = new QGridLayout;
    castling->setHorizontalSpacing(6);
    castling->setVerticalSpacing(6);
    castling->setColumnStretch(0, 1);
    for (std::size_t i = 0; i < 4; ++i) {
        m_castle[i] = chip(i % 2 == 0 ? QStringLiteral("O-O") : QStringLiteral("O-O-O"),
                           QStringLiteral("setupCastle:") + names[i]);
        m_castle[i]->setFixedWidth(i % 2 == 0 ? 58 : 72);
        connect(m_castle[i], &QPushButton::toggled, this, [this] {
            if (m_showing) {
                return;
            }
            BoardSetup setup = m_board->setup();
            setup.castling = {m_castle[0]->isChecked(), m_castle[1]->isChecked(),
                              m_castle[2]->isChecked(), m_castle[3]->isChecked()};
            m_board->setSetup(setup);
            showSetup();
        });
    }
    castling->addWidget(new QLabel(tr("White")), 0, 0);
    castling->addWidget(m_castle[0], 0, 1);
    castling->addWidget(m_castle[1], 0, 2);
    castling->addWidget(new QLabel(tr("Black")), 1, 0);
    castling->addWidget(m_castle[2], 1, 1);
    castling->addWidget(m_castle[3], 1, 2);
    box->addLayout(castling);
    box->addSpacing(10);

    // En passant and the move number, side by side.
    auto* numbers = new QGridLayout;
    numbers->setHorizontalSpacing(12);
    numbers->setVerticalSpacing(8);
    numbers->addWidget(caption(tr("EN PASSANT")), 0, 0);
    numbers->addWidget(caption(tr("MOVE")), 0, 1);
    m_enPassant = new QComboBox;
    m_enPassant->setObjectName(QStringLiteral("setupEnPassant"));
    m_enPassant->setFocusPolicy(Qt::NoFocus);
    connect(m_enPassant, &QComboBox::activated, this, [this] {
        BoardSetup setup = m_board->setup();
        const QString name = m_enPassant->currentData().toString();
        setup.enPassant = name.isEmpty() ? std::nullopt : Square::parse(name.toStdString());
        m_board->setSetup(setup);
        showSetup();
    });
    numbers->addWidget(m_enPassant, 1, 0);
    m_moveNumber = new Stepper;
    m_moveNumber->setObjectName(QStringLiteral("setupMoveNumber"));
    m_moveNumber->setRange(1, 500);
    connect(m_moveNumber, &Stepper::valueChanged, this, [this](int value) {
        if (m_showing) {
            return;
        }
        BoardSetup setup = m_board->setup();
        setup.fullmoveNumber = value;
        m_board->setSetup(setup);
        showSetup();
    });
    numbers->addWidget(m_moveNumber, 1, 1);
    numbers->setColumnStretch(0, 1);
    box->addLayout(numbers);
    box->addSpacing(18);

    // Whole boards at once.
    box->addWidget(caption(tr("BOARD")));
    auto* boards = new QGridLayout;
    boards->setSpacing(6);
    auto* start = action(tr("Starting Position"), QStringLiteral("setupStartPosition"));
    connect(start, &QPushButton::clicked, this,
            [this] { setSetup(BoardSetup::from(core::Position())); });
    auto* clear = action(tr("Empty Board"), QStringLiteral("setupClear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        BoardSetup setup = BoardSetup::empty();
        setup.sideToMove = m_board->setup().sideToMove;
        setSetup(setup);
    });
    auto* flip = action(tr("Flip Board"), QStringLiteral("setupFlip"));
    connect(flip, &QPushButton::clicked, this, [this] {
        m_board->setFlipped(!m_board->isFlipped());
        swapPalettes(); // each side's pieces stay next to its side of the board
    });
    boards->addWidget(start, 0, 0, 1, 2);
    boards->addWidget(clear, 1, 0);
    boards->addWidget(flip, 1, 1);
    box->addLayout(boards);
    box->addStretch();
    restyleTools();
    return card;
}

void PositionEditor::swapPalettes() {
    // The board's column: palette, board, palette, hint.
    QBoxLayout* column = m_boardColumn;
    column->removeWidget(m_topPalette);
    column->removeWidget(m_bottomPalette);
    std::swap(m_topPalette, m_bottomPalette);
    column->insertWidget(0, m_topPalette, 0, Qt::AlignHCenter);
    column->insertWidget(2, m_bottomPalette, 0, Qt::AlignHCenter);
}

void PositionEditor::restyleTools() {
    const auto& c = m_style.colors();
    if (m_moveTool != nullptr) {
        m_moveTool->setIcon(makeIcon(IconKind::Pointer, c.text, c.dimmed, 24));
        m_moveTool->setIconSize(QSize(24, 24));
    }
    if (m_removeTool != nullptr) {
        m_removeTool->setIcon(makeIcon(IconKind::Trash, c.text, c.dimmed, 24));
        m_removeTool->setIconSize(QSize(24, 24));
    }
}

void PositionEditor::setSetup(const BoardSetup& setup) {
    m_board->setSetup(setup);
    showSetup();
}

void PositionEditor::showSetup() {
    m_showing = true;
    const BoardSetup& setup = m_board->setup();
    m_side->button(setup.sideToMove == Color::White ? 0 : 1)->setChecked(true);

    const core::CastlingRights possible = setup.possibleCastling();
    const std::array<bool, 4> can{possible.whiteKingSide, possible.whiteQueenSide,
                                  possible.blackKingSide, possible.blackQueenSide};
    const std::array<bool, 4> wanted{setup.castling.whiteKingSide, setup.castling.whiteQueenSide,
                                     setup.castling.blackKingSide, setup.castling.blackQueenSide};
    for (std::size_t i = 0; i < 4; ++i) {
        m_castle[i]->setEnabled(can[i]);
        m_castle[i]->setChecked(can[i] && wanted[i]);
    }

    m_enPassant->clear();
    m_enPassant->addItem(tr("None"), QString());
    for (const Square square : setup.possibleEnPassant()) {
        const QString name = QString::fromStdString(square.name());
        m_enPassant->addItem(name, name);
        if (setup.enPassant == square) {
            m_enPassant->setCurrentIndex(m_enPassant->count() - 1);
        }
    }
    m_enPassant->setEnabled(m_enPassant->count() > 1);

    m_moveNumber->setValue(setup.fullmoveNumber);
    if (!m_fen->hasFocus()) {
        m_fen->setText(QString::fromStdString(setup.fen()));
    }

    QStringList problems;
    for (const core::SetupProblem problem : setup.problems()) {
        switch (problem) {
        case core::SetupProblem::NoWhiteKing:
            problems << tr("White needs a king.");
            break;
        case core::SetupProblem::NoBlackKing:
            problems << tr("Black needs a king.");
            break;
        case core::SetupProblem::TwoWhiteKings:
            problems << tr("White has more than one king.");
            break;
        case core::SetupProblem::TwoBlackKings:
            problems << tr("Black has more than one king.");
            break;
        case core::SetupProblem::PawnOnBackRank:
            problems << tr("Pawns can't stand on the first or the eighth rank.");
            break;
        case core::SetupProblem::TooManyWhitePieces:
            problems << tr("White has more than 16 pieces.");
            break;
        case core::SetupProblem::TooManyBlackPieces:
            problems << tr("Black has more than 16 pieces.");
            break;
        case core::SetupProblem::OtherSideInCheck:
            problems << (setup.sideToMove == Color::White
                             ? tr("Black is in check, so it can't be White's move.")
                             : tr("White is in check, so it can't be Black's move."));
            break;
        }
    }
    m_problems->setText(problems.join(QLatin1Char(' ')));
    // Shown or empty, it keeps its room: the buttons stay at the right.
    m_start->setEnabled(problems.isEmpty());
    m_showing = false;
}

std::optional<core::Position> PositionEditor::position() const {
    return m_board->setup().position();
}

} // namespace kiebitz::app
