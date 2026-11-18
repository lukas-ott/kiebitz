// Zen mode: only the training, full screen. The sidebar, the game tabs and the board's panels
// (engine, explorer) go, the pages can't be switched to, and self-study shows nothing but its
// clock. Esc leaves it, and so does the end of the session.

#include "MainWindow.hpp"
#include "training/TrainingSession.hpp"
#include "training/TrainingView.hpp"

#include <QAction>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>

namespace kiebitz::app {

void MainWindow::setZen(bool zen) {
    if (zen == m_zen || (zen && !m_training->running())) {
        return;
    }
    m_zen = zen;
    if (zen) {
        m_beforeZen = windowState();
        showFullScreen();
    } else {
        setWindowState(m_beforeZen);
    }
    m_sidebarFrame->setVisible(!zen);
    m_tabRow->setVisible(!zen);
    m_panelsCard->setVisible(!zen);
    for (QAction* action : m_sectionActions) {
        action->setEnabled(!zen);
    }
    m_zenEscape->setEnabled(zen);
    m_trainingBar->setZen(zen);
    if (zen) {
        // Where the training is: the part's page, unless it's there already (a repertoire file
        // being practised on the board counts as the openings part's).
        const training::Part part = m_training->session()->part();
        const bool there = (part == training::Part::Openings &&
                            (m_section == Section::Library || m_section == Section::Board)) ||
                           (part != training::Part::Openings && part != training::Part::SelfStudy &&
                            m_section == Section::Puzzles);
        if (!there) {
            showTrainingPart(part);
        }
    }
    showZenPage();
}

void MainWindow::showZenPage() {
    const training::Session* session = m_training->session();
    const bool clock = m_zen && session != nullptr && session->part() == training::Part::SelfStudy;
    // Zen: no status bar. Self-study: the clock alone, not even the training bar over it.
    statusBar()->setVisible(!m_zen);
    m_trainingBar->setCovered(clock);
    updatePage();
}

} // namespace kiebitz::app
