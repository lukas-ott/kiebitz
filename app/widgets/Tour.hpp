#pragma once

#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QPushButton;

namespace kiebitz::app {

class AppStyle;

/// One stop of a tour: what it says, the part of the window it points at, and what to show
/// first (another page).
struct TourStep {
    QString title;
    QString text;
    /// The widget it points at (null, or hidden: the card sits in the middle, nothing is
    /// singled out). Asked again whenever the window changes.
    std::function<QWidget*()> target;
    /// Before it shows: switch to the page it is about.
    std::function<void()> enter;
    /// An optional button on the card ("Create These Folders"): its text, and what it does,
    /// which returns the text the button shows afterwards (empty: it goes away).
    QString actionText;
    std::function<QString()> action;
};

/// A short guided tour over a window: everything but the part a step is about is dimmed, and a
/// card beside that part explains it, with Back, Next and Skip. Esc or Skip ends it at once.
///
/// It covers the window as a child of it and takes the keys while it is up (← → ⏎ Esc), so the
/// window's own shortcuts don't act underneath.
class Tour : public QWidget {
    Q_OBJECT

public:
    Tour(const AppStyle& style, QWidget& window, std::vector<TourStep> steps);

    /// Shows the first step.
    void start();
    void next();
    void back();
    /// Ends it now (the same as Done at the end).
    void finish();
    [[nodiscard]] int step() const noexcept { return m_step; }
    [[nodiscard]] int stepCount() const noexcept { return static_cast<int>(m_steps.size()); }
    /// Where the card is and what it points at (for tests): in the tour's coordinates.
    [[nodiscard]] QRect cardRect() const;
    [[nodiscard]] QRect spotlight() const { return m_spot; }

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    /// It ended; `completed`: the last step was reached (not skipped before).
    void finished(bool completed);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void show(int step);
    /// Places the spotlight and the card for the step shown (after a resize too).
    void place();
    /// Places them again if the target, or where it is, changed.
    void follow();

    const AppStyle& m_style;
    QWidget& m_window;
    std::vector<TourStep> m_steps;
    int m_step = -1;
    QPointer<QWidget> m_target;
    QRect m_targetRect; ///< where it was when placed (in the window)
    QTimer m_follow;    ///< looks again now and then: pages change what they show
    QRect m_spot;       ///< the part singled out (empty: none)

    QWidget* m_card = nullptr;
    QLabel* m_count = nullptr; ///< "2 of 6"
    QLabel* m_title = nullptr;
    QLabel* m_text = nullptr;
    QPushButton* m_action = nullptr;
    QPushButton* m_back = nullptr;
    QPushButton* m_next = nullptr;
    QPushButton* m_skip = nullptr;
};

} // namespace kiebitz::app
