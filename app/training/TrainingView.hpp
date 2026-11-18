#pragma once

#include "TrainingHistory.hpp"
#include "kiebitz/training/session.hpp"

#include <QWidget>

#include <array>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QToolButton;

namespace kiebitz::app {

class ActivityGrid;
class AppStyle;
class PageHeader;
class Stepper;
class Toggle;
class TrainingSession;

/// The Training page: the plan (minutes per part, which parts), Start, and what you have
/// trained: today, the streak, and a GitHub-like grid of the days.
class TrainingView : public QWidget {
    Q_OBJECT

public:
    TrainingView(TrainingSession& session, TrainingHistory& stats, const AppStyle& style,
                 QWidget* parent = nullptr);

    /// The page is shown: the numbers read again.
    void activate();

signals:
    /// "Zen Mode": the plan (started if it isn't running) with nothing else of the app around.
    void zenRequested();

public:
    // For tests
    [[nodiscard]] ActivityGrid* grid() const { return m_grid; }

private:
    void savePlan();
    void showSession();
    void showStats(const TrainingHistory::Summary& summary);

    TrainingSession& m_session;
    TrainingHistory& m_stats;
    const AppStyle& m_style;
    PageHeader* m_header = nullptr;
    QPushButton* m_start = nullptr;
    struct Row {
        QWidget* frame = nullptr;
        Toggle* on = nullptr;
        Stepper* minutes = nullptr;
        QLabel* left = nullptr; ///< "12:34 left" while it's the part running
    };
    std::array<Row, training::kParts> m_rows{};
    QComboBox* m_side = nullptr; ///< the openings part: which repertoire
    QLabel* m_total = nullptr;
    std::array<QLabel*, 4> m_tiles{}; ///< today, streak, best streak, the last 7 days
    struct PartBar {
        QLabel* name = nullptr;
        QProgressBar* bar = nullptr;
        QLabel* value = nullptr; ///< "8 / 15 min"
    };
    std::array<PartBar, training::kParts> m_partBars{}; ///< today, part by part
    ActivityGrid* m_grid = nullptr;
    QComboBox* m_year = nullptr; ///< the last 12 months, or a year (once there are several)
    TrainingHistory::Summary m_summary;
};

/// The running session over every page: the part, its time left in large figures, and Pause,
/// Next, Stop and Zen.
class TrainingBar : public QWidget {
    Q_OBJECT

public:
    TrainingBar(TrainingSession& session, const AppStyle& style, QWidget* parent = nullptr);
    [[nodiscard]] QString text() const; ///< tests

signals:
    /// The part's name was clicked: show the Training page.
    void showRequested();
    /// "Zen" clicked: only the training, nothing else of the app.
    void zenRequested();

public:
    /// In Zen mode already: its button goes.
    void setZen(bool zen);
    /// Zen mode's self-study clock shows the time instead: the bar goes meanwhile.
    void setCovered(bool covered);

private:
    void refresh();

    TrainingSession& m_session;
    const AppStyle& m_style;
    QToolButton* m_part = nullptr;
    QLabel* m_time = nullptr;   ///< "12:34", large
    QLabel* m_detail = nullptr; ///< "left", "next: Tactics", "paused"
    QString m_text;             ///< all of it in one line (tests)
    bool m_covered = false;
    QToolButton* m_pause = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_stop = nullptr;
    QToolButton* m_zen = nullptr;
    bool m_inZen = false;
};

/// Zen mode's self-study: nothing but the time left, large, the part's name over it, and Pause
/// and Next under it.
class ZenClock : public QWidget {
    Q_OBJECT

public:
    explicit ZenClock(TrainingSession& session, QWidget* parent = nullptr);
    [[nodiscard]] QString time() const; ///< tests

private:
    void refresh();

    TrainingSession& m_session;
    QLabel* m_part = nullptr;
    QLabel* m_time = nullptr;
    QPushButton* m_pause = nullptr;
    QPushButton* m_next = nullptr;
};

/// "12:34", "1:02:03".
[[nodiscard]] QString clockText(qint64 ms);

} // namespace kiebitz::app
