#include "MainWindow.hpp"
#include "board/BoardView.hpp"
#include "document/GameDocument.hpp"
#include "library/Library.hpp"
#include "notation/NotationView.hpp"
#include "repertoire/RepertoireStore.hpp"
#include "style/AppStyle.hpp"
#include "widgets/Tour.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabBar>
#include <QTest>
#include <QToolButton>
#include <gtest/gtest.h>

#include <algorithm>

namespace {

using kiebitz::app::BoardView;
using kiebitz::app::MainWindow;
using kiebitz::app::NotationView;
using namespace kiebitz::core;

/// Finds a menu action by its text (without the & mnemonic).
QAction* action(MainWindow& window, const QString& text) {
    for (auto* candidate : window.findChildren<QAction*>()) {
        if (candidate->text().remove(QLatin1Char('&')) == text) {
            return candidate;
        }
    }
    return nullptr;
}

class MainWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
        board = window.findChild<BoardView*>(QStringLiteral("gameBoard"));
        ASSERT_NE(board, nullptr);
        // The app starts in the Library; the game tests work on the board.
        window.showSection(MainWindow::Section::Board);
    }
    void play(std::string_view san) {
        const Move move = board->position().parseSan(san).value();
        emit board->moveMade(move);
    }
    [[nodiscard]] std::string fen() const { return board->position().fen(); }

    kiebitz::app::AppStyle style{*qApp};
    MainWindow window{style};
    BoardView* board = nullptr;
};

TEST_F(MainWindowTest, MovesAreAddedAndCanBeNavigated) {
    play("e4");
    play("e5");
    play("Nf3");
    const std::string afterNf3 = fen();

    action(window, QStringLiteral("Back"))->trigger();
    action(window, QStringLiteral("Back"))->trigger();
    EXPECT_EQ(fen(), "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");

    action(window, QStringLiteral("Go to End"))->trigger();
    EXPECT_EQ(fen(), afterNf3);
    EXPECT_FALSE(action(window, QStringLiteral("Forward"))->isEnabled());

    action(window, QStringLiteral("Go to Start"))->trigger();
    EXPECT_EQ(fen(), std::string(kStartFen));
    EXPECT_FALSE(action(window, QStringLiteral("Back"))->isEnabled());

    action(window, QStringLiteral("Forward"))->trigger();
    EXPECT_EQ(fen(), "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
}

TEST_F(MainWindowTest, DrawingsStayWithTheirMove) {
    play("e4");
    emit board->drawingsChanged(
        {{Square::parse("d2").value(), Square::parse("d4").value(), MarkColor::Green}}, {});
    play("e5");
    EXPECT_TRUE(board->arrows().empty()); // a new move has no drawings yet
    action(window, QStringLiteral("Back"))->trigger();
    EXPECT_EQ(board->arrows().size(), 1U); // back on 1. e4: its arrow is shown again
}

TEST_F(MainWindowTest, FirstLaunchFillsTheScreen) {
    // No saved window geometry (the tests' settings are empty): zoomed, but not full screen.
    EXPECT_TRUE(window.windowState().testFlag(Qt::WindowMaximized));
    EXPECT_FALSE(window.windowState().testFlag(Qt::WindowFullScreen));
}

TEST_F(MainWindowTest, TabsAreNeverSqueezedByASmallWindow) {
    auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    ASSERT_NE(tabs, nullptr);
    window.setWindowState(Qt::WindowNoState);
    window.resize(200, 200); // far too small: the window stops at its minimum size
    QApplication::processEvents();
    EXPECT_GE(window.width(), 960);
    EXPECT_GE(tabs->width(), tabs->sizeHint().width()); // every tab in full, no scroll arrows
}

TEST_F(MainWindowTest, NewTabStartsAFreshGame) {
    play("d4");
    const auto* tabs = window.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    const int before = tabs->count();
    action(window, QStringLiteral("New Tab"))->trigger();
    EXPECT_EQ(tabs->count(), before + 1);
    EXPECT_EQ(fen(), std::string(kStartFen));
    // An untouched new game is reused instead of piling up empty tabs.
    action(window, QStringLiteral("New Tab"))->trigger();
    EXPECT_EQ(tabs->count(), before + 1);
}

TEST_F(MainWindowTest, FlipActionFlipsTheBoard) {
    const bool before = board->isFlipped();
    action(window, QStringLiteral("Flip Board"))->trigger();
    EXPECT_NE(board->isFlipped(), before);
    action(window, QStringLiteral("Flip Board"))->trigger();
    EXPECT_EQ(board->isFlipped(), before);
}

TEST_F(MainWindowTest, MoveListShowsMovesVariationsAndNumbers) {
    play("e4");
    play("e5");
    action(window, QStringLiteral("Back"))->trigger();
    play("c5"); // a variation at Black's first move
    play("Nf3");
    auto* list = window.findChild<NotationView*>(QStringLiteral("moveList"));
    ASSERT_NE(list, nullptr);
    const QString text = list->plainText().simplified().replace(QChar(0xA0), QLatin1Char(' '));
    // First-level variations get their own indented line, so no brackets here.
    EXPECT_EQ(text, QStringLiteral("1. e4 e5 1… c5 2. Nf3"));
}

TEST_F(MainWindowTest, ClickingAMoveInTheListGoesThere) {
    play("d4");
    play("d5");
    auto* list = window.findChild<NotationView*>(QStringLiteral("moveList"));
    ASSERT_NE(list, nullptr);
    const QRect d4 = list->moveRect(1); // node 1 = 1. d4
    ASSERT_FALSE(d4.isEmpty());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, d4.center());
    EXPECT_EQ(fen(), "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1");
}

/// Saves the whole window in dark and light when KIEBITZ_SNAPSHOT_DIR is set.
TEST_F(MainWindowTest, Snapshots) {
    window.resize(1200, 760);
    for (const char* san : {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6", "O-O", "Be7"}) {
        play(san);
    }
    action(window, QStringLiteral("Back"))->trigger();
    action(window, QStringLiteral("Back"))->trigger();
    play("d3");
    emit board->drawingsChanged(
        {{Square::parse("c2").value(), Square::parse("c3").value(), MarkColor::Green},
         {Square::parse("b1").value(), Square::parse("d2").value(), MarkColor::Blue}},
        {});
    board->setDrawings(
        {{Square::parse("c2").value(), Square::parse("c3").value(), MarkColor::Green},
         {Square::parse("b1").value(), Square::parse("d2").value(), MarkColor::Blue}},
        {});
    QTest::keyClick(&window, Qt::Key_Left); // back to 4... Nf6: a branch point (5. O-O or 5. d3)
    QTest::keyClick(&window, Qt::Key_Down); // choose 5. d3
    const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR");
    // The default dark and light themes, then every theme, to look at side by side.
    const auto save = [&](const QString& name) {
        QApplication::processEvents();
        const QImage image = window.grab().toImage();
        EXPECT_FALSE(image.isNull());
        if (!dir.isEmpty()) {
            image.save(QDir(dir).filePath(name));
        }
    };
    style.setTheme(QStringLiteral("graphite"));
    save(QStringLiteral("window-dark.png"));
    style.setTheme(QStringLiteral("daylight"));
    save(QStringLiteral("window-light.png"));
    for (const auto& theme : kiebitz::app::AppStyle::themes()) {
        style.setTheme(theme.id);
        save(QStringLiteral("app-theme-%1.png").arg(theme.id));
    }
    style.setTheme({});
}

/// The keys themselves, not just the menu items: on macOS plain-key menu shortcuts never fire,
/// so the window handles them.
TEST_F(MainWindowTest, ArrowKeysNavigate) {
    play("e4");
    play("e5");
    play("Nf3");
    const std::string afterNf3 = fen();

    QTest::keyClick(&window, Qt::Key_Left);
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_EQ(fen(), "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    QTest::keyClick(&window, Qt::Key_Right);
    QTest::keyClick(&window, Qt::Key_Right);
    EXPECT_EQ(fen(), afterNf3);
    QTest::keyClick(&window, Qt::Key_Right); // at the end: nothing happens
    EXPECT_EQ(fen(), afterNf3);
    QTest::keyClick(&window, Qt::Key_Home);
    EXPECT_EQ(fen(), std::string(kStartFen));
    QTest::keyClick(&window, Qt::Key_End);
    EXPECT_EQ(fen(), afterNf3);

    // Also while the board has focus.
    board->setFocus();
    QTest::keyClick(board, Qt::Key_Left);
    EXPECT_NE(fen(), afterNf3);
}

TEST_F(MainWindowTest, TJumpsToTheNextTransposition) {
    play("e4");
    play("e5");
    play("Nf3");
    const std::string position = fen();
    QTest::keyClick(&window, Qt::Key_Home);
    play("Nf3"); // 1. Nf3 e5 2. e4: the same position by another move order
    play("e5");
    play("e4");
    // The same but for the move counters (a pawn moved last here).
    const auto placement = [](const std::string& text) {
        return text.substr(0, text.size() - 4);
    };
    ASSERT_EQ(placement(fen()), placement(position));
    const std::string afterNf3e5 = "rnbqkbnr/pppp1ppp/8/4p3/8/5N2/PPPPPPPP/RNBQKB1R w KQkq - 0 2";

    QTest::keyClick(&window, Qt::Key_T); // to 2. Nf3 in the main line
    EXPECT_EQ(fen(), position);
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_EQ(fen(), "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2");
    QTest::keyClick(&window, Qt::Key_Right);
    QTest::keyClick(&window, Qt::Key_T); // and back to 2. e4 in the variation
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_EQ(fen(), afterNf3e5);

    // A position reached once: T stays.
    QTest::keyClick(&window, Qt::Key_T);
    EXPECT_EQ(fen(), afterNf3e5);

    // The list marks both 2. Nf3 (node 3) and 2. e4 (node 6) with a ⇄; a click on it goes to
    // the other one, a click on the move itself to the move.
    auto* list = window.findChild<NotationView*>(QStringLiteral("moveList"));
    ASSERT_NE(list, nullptr);
    const QRect nf3 = list->moveRect(3);
    const QRect e4 = list->moveRect(6);
    ASSERT_FALSE(nf3.isEmpty());
    ASSERT_FALSE(e4.isEmpty());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      QPoint(e4.right() - 5, e4.center().y()));
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_EQ(fen(), "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"); // main line
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(e4.left() + 4, e4.center().y()));
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_EQ(fen(), afterNf3e5); // the move itself: still the variation
    if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
        window.resize(1200, 760);
        QTest::qWait(100);
        list->grab().save(QDir(dir).filePath(QStringLiteral("notation-transpositions.png")));
    }
}

TEST_F(MainWindowTest, TheIntroTourShowsEachPartAndCanBeSkipped) {
    window.resize(1200, 760);
    play("e4");
    const std::string afterE4 = fen();
    window.startTourOnce(); // the first time: it shows
    auto* tour = window.findChild<kiebitz::app::Tour*>();
    ASSERT_NE(tour, nullptr);
    ASSERT_TRUE(tour->isVisible());
    const auto click = [&](const char* name) {
        auto* button = tour->findChild<QPushButton*>(QString::fromLatin1(name));
        ASSERT_NE(button, nullptr) << name;
        ASSERT_TRUE(button->isVisible()) << name;
        QTest::mouseClick(button, Qt::LeftButton);
    };
    const auto snapshot = [&](int step) {
        if (const QString dir = qEnvironmentVariable("KIEBITZ_SNAPSHOT_DIR"); !dir.isEmpty()) {
            QTest::qWait(150);
            window.grab().save(QDir(dir).filePath(QStringLiteral("tour-%1.png").arg(step)));
        }
    };
    // The welcome: in the middle, nothing singled out; no Back yet.
    EXPECT_EQ(tour->step(), 0);
    EXPECT_TRUE(tour->spotlight().isEmpty());
    EXPECT_FALSE(tour->findChild<QPushButton*>(QStringLiteral("tourBack"))->isVisible());
    // The window's keys wait while it is up: ← doesn't take the move back.
    QTest::keyClick(tour, Qt::Key_Left);
    EXPECT_EQ(fen(), afterE4);
    snapshot(0);

    // The Library, its files singled out, the card beside them.
    const QString yours = window.library().folder();
    click("tourNext");
    EXPECT_EQ(tour->step(), 1);
    EXPECT_EQ(window.section(), MainWindow::Section::Library);
    // A new Library is empty: the tour shows a sample one meanwhile, its files singled out.
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            const auto* files = window.findChild<QWidget*>(QStringLiteral("libraryFiles"));
            const auto& all = window.library().files();
            return files->isVisible() && all.size() == 4 &&
                   std::all_of(all.begin(), all.end(), [](const auto& file) {
                       return file.pgn != nullptr;
                   }); // read, not only found
        },
        5000));
    EXPECT_NE(window.library().folder(), yours);
    const auto* files = window.findChild<QWidget*>(QStringLiteral("libraryFiles"));
    const QRect filesRect(files->mapTo(&window, QPoint(0, 0)), files->size());
    EXPECT_TRUE(
        QTest::qWaitFor([&] { return tour->spotlight().contains(filesRect.center()); }, 5000));
    EXPECT_FALSE(tour->cardRect().intersects(filesRect));
    // Every sample reads, each the kind its folder says.
    for (const auto& file : window.library().files()) {
        ASSERT_TRUE(file.pgn) << file.name.toStdString();
        EXPECT_FALSE(file.pgn->games.empty()) << file.name.toStdString();
    }
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return window.repertoires().files(kiebitz::core::Color::White).size() == 1 &&
                   window.repertoires().files(kiebitz::core::Color::Black).size() == 1;
        },
        5000)); // Openings/White and Openings/Black: one opening file each
    // The card is tall enough for all its text.
    const auto* text = tour->findChild<QLabel*>(QStringLiteral("tourText"));
    EXPECT_GE(text->height(), text->heightForWidth(text->width()));
    snapshot(1);
    // Yours has none of the suggested folders: the tour makes them, in yours.
    const QDir library(yours);
    ASSERT_FALSE(library.exists(QStringLiteral("Openings")));
    click("tourAction");
    EXPECT_TRUE(library.exists(QStringLiteral("Openings/White")));
    EXPECT_TRUE(library.exists(QStringLiteral("Openings/Black")));
    EXPECT_TRUE(library.exists(QStringLiteral("Games")));
    EXPECT_TRUE(library.exists(QStringLiteral("Puzzles")));
    EXPECT_FALSE(tour->findChild<QPushButton*>(QStringLiteral("tourAction"))->isEnabled());

    // Every other step, by key and back and forth, each on its own page.
    QTest::keyClick(tour, Qt::Key_Right);
    snapshot(2);
    QTest::keyClick(tour, Qt::Key_Left);
    EXPECT_EQ(tour->step(), 1);
    QTest::keyClick(tour, Qt::Key_Right);
    click("tourNext");
    EXPECT_EQ(window.section(), MainWindow::Section::Board);
    EXPECT_FALSE(tour->spotlight().isEmpty());
    // A sample game on the board, with a transposition in it.
    ASSERT_TRUE(QTest::qWaitFor(
        [&] {
            return window.document() != nullptr && window.document()->file &&
                   window.document()->file->path.endsWith(u"Italian Game.pgn");
        },
        5000));
    EXPECT_FALSE(window.document()->game.transposedNodes().empty());
    snapshot(3);
    click("tourNext");
    EXPECT_EQ(window.section(), MainWindow::Section::Profiles);
    snapshot(4);
    click("tourNext");
    EXPECT_EQ(window.section(), MainWindow::Section::Training);
    EXPECT_EQ(tour->step(), tour->stepCount() - 1);
    EXPECT_FALSE(tour->findChild<QPushButton*>(QStringLiteral("tourSkip"))->isVisible());
    snapshot(5);

    // Esc ends it; the window is back where it was, and it never comes back by itself.
    QSignalSpy finished(tour, &kiebitz::app::Tour::finished);
    QTest::keyClick(tour, Qt::Key_Escape);
    ASSERT_EQ(finished.size(), 1);
    EXPECT_TRUE(finished.front().at(0).toBool()); // it got to the end
    EXPECT_EQ(window.section(), MainWindow::Section::Board);
    // The samples are gone: your Library is back, the sample game's tab closed.
    EXPECT_EQ(window.library().folder(), yours);
    EXPECT_TRUE(window.document() == nullptr || !window.document()->file ||
                !window.document()->file->path.endsWith(u"Italian Game.pgn"));
    QTest::qWait(50);
    EXPECT_EQ(window.findChild<kiebitz::app::Tour*>(), nullptr);
    window.startTourOnce();
    EXPECT_EQ(window.findChild<kiebitz::app::Tour*>(), nullptr);

    // From the Help menu: always. Skip ends it at once.
    action(window, QStringLiteral("Intro Tour"))->trigger();
    tour = window.findChild<kiebitz::app::Tour*>();
    ASSERT_NE(tour, nullptr);
    QSignalSpy skipped(tour, &kiebitz::app::Tour::finished);
    click("tourSkip");
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_FALSE(skipped.front().at(0).toBool());
}

TEST_F(MainWindowTest, FKeyFlipsTheBoard) {
    const bool before = board->isFlipped();
    QTest::keyClick(&window, Qt::Key_F);
    EXPECT_NE(board->isFlipped(), before);
    QTest::keyClick(&window, Qt::Key_F);
    EXPECT_EQ(board->isFlipped(), before);
}

TEST_F(MainWindowTest, UpAndDownJumpToStartAndEnd) {
    play("d4");
    play("d5");
    play("c4");
    const std::string end = fen();
    QTest::keyClick(&window, Qt::Key_Up);
    EXPECT_EQ(fen(), std::string(kStartFen));
    QTest::keyClick(&window, Qt::Key_Down);
    EXPECT_EQ(fen(), end);
}

TEST_F(MainWindowTest, UpAndDownChooseBetweenContinuations) {
    play("e4");
    play("e5");
    QTest::keyClick(&window, Qt::Key_Left);
    play("c5"); // second continuation after 1. e4
    QTest::keyClick(&window, Qt::Key_Left);
    play("e6");                             // third
    QTest::keyClick(&window, Qt::Key_Left); // back on 1. e4: three continuations

    // The board shows them as candidate arrows; the list offers them too.
    const auto buttons = window.findChildren<QPushButton*>();
    const auto forks = std::count_if(buttons.begin(), buttons.end(), [](QPushButton* button) {
        return button->property("role").toString() == QLatin1String("fork") && button->isVisible();
    });
    EXPECT_EQ(forks, 3);

    QTest::keyClick(&window, Qt::Key_Down); // choose 1... c5
    QTest::keyClick(&window, Qt::Key_Right);
    EXPECT_EQ(fen(), "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2");

    QTest::keyClick(&window, Qt::Key_Left);
    QTest::keyClick(&window, Qt::Key_Up); // wraps around to the last: 1... e6
    QTest::keyClick(&window, Qt::Key_Right);
    EXPECT_EQ(fen(), "rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2");

    QTest::keyClick(&window, Qt::Key_Left);
    QTest::keyClick(&window, Qt::Key_Right); // no choice made: the main line
    EXPECT_EQ(fen(), "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2");
}

TEST_F(MainWindowTest, ContinuationHintShowsOnlyUntilFirstUse) {
    QSettings().remove(QStringLiteral("hints/continuationsLearned"));
    const auto hintVisible = [this] {
        const auto labels = window.findChildren<QLabel*>();
        return std::any_of(labels.begin(), labels.end(), [](QLabel* label) {
            return label->property("role").toString() == QLatin1String("hint") &&
                   label->isVisible();
        });
    };
    play("e4");
    QTest::keyClick(&window, Qt::Key_Left);
    play("d4");
    QTest::keyClick(&window, Qt::Key_Left); // a branch point for the first time
    EXPECT_TRUE(hintVisible());

    QTest::keyClick(&window, Qt::Key_Down); // the user chooses once...
    QTest::keyClick(&window, Qt::Key_Right);
    QTest::keyClick(&window, Qt::Key_Left);
    EXPECT_FALSE(hintVisible()); // ...and the hint is gone for good
}

} // namespace
