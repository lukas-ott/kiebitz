// The intro tour: Help → Intro Tour, and once on the first launch.

#include "MainWindow.hpp"
#include "document/Documents.hpp"
#include "library/Library.hpp"
#include "widgets/Tour.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <initializer_list>
#include <memory>
#include <vector>

namespace kiebitz::app {
namespace {

constexpr QLatin1StringView kTourShown{"tour/shown"};

/// The folders the tour suggests, at the top of the Library. In English whatever the language:
/// the Library knows these names (and their German and Norwegian ones).
const QStringList& starterFolders() {
    static const QStringList folders{QStringLiteral("Openings/White"),
                                     QStringLiteral("Openings/Black"), QStringLiteral("Games"),
                                     QStringLiteral("Puzzles")};
    return folders;
}

/// The sample Library, copied out of the app into a folder of its own that goes when the tour
/// ends: a few opening files, a model game and two puzzles.
std::shared_ptr<QTemporaryDir> sampleLibrary() {
    auto folder = std::make_shared<QTemporaryDir>(
        QDir(QDir::tempPath()).filePath(QStringLiteral("Sample Library-XXXXXX")));
    if (!folder->isValid()) {
        return nullptr;
    }
    const QString from = QStringLiteral(":/tour/library");
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = folder->filePath(source.mid(from.size() + 1));
        QDir().mkpath(QFileInfo(target).path());
        QFile::copy(source, target);
        // Copied out of the app read-only: writable, so a sample game can be played on.
        QFile::setPermissions(target, QFile::permissions(target) | QFileDevice::WriteOwner);
    }
    return folder;
}

} // namespace

void MainWindow::startTourOnce() {
    if (QSettings().value(kTourShown).toBool()) {
        return;
    }
    startTour();
}

void MainWindow::startTour() {
    if (findChild<Tour*>() != nullptr) {
        return; // already showing
    }
    QSettings().setValue(kTourShown, true); // skipped counts too: it never comes back unasked
    const Section before = m_section;
    const QString realFolder = m_library->folder();
    // Nothing in the Library yet: the tour shows the sample one meanwhile (never written into
    // the settings, so yours is back even if Kiebitz quits during the tour).
    const bool showSample = m_library->files().empty();
    const std::shared_ptr<QTemporaryDir> sample = sampleLibrary();

    // The first of these widgets that is showing.
    const auto widget = [this](std::initializer_list<const char*> names) {
        return [this, list = std::vector<const char*>(names)]() -> QWidget* {
            for (const char* name : list) {
                auto* found = findChild<QWidget*>(QString::fromLatin1(name));
                if (found != nullptr && found->isVisible()) {
                    return found;
                }
            }
            return nullptr;
        };
    };
    const auto go = [this](Section section) {
        return [this, section] {
            showSection(section);
        };
    };
    const auto library = [this, sample, showSample] {
        if (showSample && sample && m_library->folder() != QDir::cleanPath(sample->path())) {
            m_library->setFolder(sample->path());
        }
        showSection(Section::Library);
    };

    QString shownFolder = QDir::toNativeSeparators(realFolder);
    if (const QString home = QDir::homePath(); shownFolder.startsWith(home)) {
        shownFolder = u'~' + shownFolder.mid(home.size());
    }
    QStringList missing;
    for (const QString& name : starterFolders()) {
        if (!QDir(realFolder).exists(name)) {
            missing << name;
        }
    }

    std::vector<TourStep> steps;
    steps.push_back(
        {tr("Welcome to Kiebitz"),
         tr("Kiebitz is for opening preparation: build your <b>repertoire</b> in PGN files, see "
            "<b>where your games left it</b>, <b>prepare for your next opponent</b> from their "
            "games, and <b>train</b> the lines and positions you missed.<br><br>This tour shows "
            "where each part lives. It is how Kiebitz was meant to be used: a suggestion, not a "
            "rule."),
         nullptr, nullptr, QString(), nullptr});
    TourStep libraryStep{
        tr("Your Library"),
        tr("Every PGN file in your Library folder, %1. A layout that works well:<br>"
           "• <b>Openings/White</b>, <b>Openings/Black</b>: your opening files, a folder per "
           "repertoire if you like<br>"
           "• <b>Games</b>: your own games and model games<br>"
           "• <b>Puzzles</b>: puzzle files<br><br>"
           "Files there take their type and side from the folder.")
                .arg(shownFolder.toHtmlEscaped()) +
            (showSample ? QStringLiteral("<br><br>") +
                              tr("<i>Shown here: a sample Library, only during the tour.</i>")
                        : QString()),
        widget({"libraryFiles", "libraryEmpty"}),
        library,
        QString(),
        nullptr};
    if (!missing.isEmpty()) {
        libraryStep.actionText = tr("Create These Folders");
        libraryStep.action = [this, missing, realFolder] {
            for (const QString& name : missing) {
                QDir(realFolder).mkpath(name);
            }
            if (m_library->folder() == realFolder) {
                m_library->rescan();
            }
            return tr("Folders created");
        };
    }
    steps.push_back(std::move(libraryStep));
    steps.push_back(
        {tr("Openings and your repertoire"),
         tr("Every opening file is an <b>Opening</b>. Your <b>repertoire</b> is the openings you "
            "play now: right-click a folder or a file and turn <b>In Repertoire</b> on or off. "
            "The others stay in the Library, dimmed; practice, the theory check and the Explorer "
            "leave them out.<br><br>Right-click a file to set its type by hand."),
         widget({"libraryFiles", "libraryEmpty"}), library, QString(), nullptr});
    bool sampleGameOpened = false;
    steps.push_back(
        {tr("Studying a line"),
         tr("A file you open shows on the Board. Play moves to add them; the Explorer shows "
            "what is played from here.<br><br>A <b>⇄</b> after a move means its position also "
            "comes up by another move order: click it, or press T, to go there."),
         widget({"moveList"}),
         [this, sample, sampleGameOpened]() mutable {
             // A sample game to look at (with a transposition), closed again after the tour.
             if (sample && !sampleGameOpened) {
                 sampleGameOpened = true;
                 m_documents->openFile(
                     sample->filePath(QStringLiteral("Openings/White/Italian Game.pgn")));
             }
             showSection(Section::Board);
         },
         QString(), nullptr});
    steps.push_back(
        {tr("You and your opponents"),
         tr("Your profile gets your games from Lichess and chess.com when you sync it; your "
            "over-the-board games you add yourself (Add PGN…, Paste Games). Its "
            "<b>Repertoire</b> tab shows where each game left your book and, after a sync, "
            "which games are new.<br><br>Add a profile for an opponent: its <b>Prepare</b> tab "
            "shows their lines next to yours, and where your repertoire has gaps."),
         widget({"profileList"}), go(Section::Profiles), QString(), nullptr});
    steps.push_back(
        {tr("A little every day"),
         tr("Training runs one plan in order: your repertoire lines, then puzzles by theme. "
            "It keeps your minutes and your streak.<br><br>Take this tour again any time from "
            "Help → Intro Tour."),
         widget({"trainingPlan"}), go(Section::Training), QString(), nullptr});

    auto* tour = new Tour(m_style, *this, std::move(steps));
    connect(tour, &Tour::finished, this, [this, before, sample, realFolder] {
        // The samples go: their tab, and the sample Library (yours is back).
        if (sample) {
            m_documents->closeUnder(QDir::cleanPath(sample->path()));
        }
        if (m_library->folder() != QDir::cleanPath(realFolder)) {
            m_library->setFolder(realFolder);
        }
        showSection(before);
    });
    tour->start();
}

} // namespace kiebitz::app
