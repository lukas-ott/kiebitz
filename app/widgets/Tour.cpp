#include "Tour.hpp"

#include "style/AppStyle.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace kiebitz::app {
namespace {

constexpr int kCardWidth = 340;
constexpr int kGap = 14;    ///< between the spotlight and the card
constexpr int kMargin = 12; ///< the card keeps this far from the window's edges
constexpr int kPad = 6;     ///< the spotlight reaches this far around its widget
constexpr qreal kRadius = 10;

} // namespace

Tour::Tour(const AppStyle& style, QWidget& window, std::vector<TourStep> steps)
    : QWidget(&window), m_style(style), m_window(window), m_steps(std::move(steps)) {
    setObjectName(QStringLiteral("tour"));
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    hide();
    window.installEventFilter(this);
    m_follow.setInterval(150);
    connect(&m_follow, &QTimer::timeout, this, &Tour::follow);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("tourCard"));
    m_card->setFixedWidth(kCardWidth);
    auto* layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(8);
    m_count = new QLabel;
    m_count->setObjectName(QStringLiteral("tourCount"));
    m_count->setProperty("role", "emptyHint");
    layout->addWidget(m_count);
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("tourTitle"));
    m_title->setProperty("role", "sectionTitle");
    m_title->setWordWrap(true);
    layout->addWidget(m_title);
    m_text = new QLabel;
    m_text->setObjectName(QStringLiteral("tourText"));
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::RichText);
    layout->addWidget(m_text);
    m_action = new QPushButton;
    m_action->setObjectName(QStringLiteral("tourAction"));
    m_action->setProperty("role", "secondary");
    m_action->setFocusPolicy(Qt::NoFocus);
    connect(m_action, &QPushButton::clicked, this, [this] {
        const TourStep& step = m_steps[static_cast<std::size_t>(m_step)];
        if (!step.action) {
            return;
        }
        const QString after = step.action();
        m_action->setText(after);
        m_action->setEnabled(false);
        m_action->setVisible(!after.isEmpty());
        place();
    });
    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(m_action);
    actionRow->addStretch();
    layout->addLayout(actionRow);
    layout->addSpacing(6);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_skip = new QPushButton(tr("Skip Tour"));
    m_skip->setObjectName(QStringLiteral("tourSkip"));
    m_skip->setProperty("role", "link");
    m_skip->setFocusPolicy(Qt::NoFocus);
    connect(m_skip, &QPushButton::clicked, this, &Tour::finish);
    buttons->addWidget(m_skip);
    buttons->addStretch();
    m_back = new QPushButton(tr("Back"));
    m_back->setObjectName(QStringLiteral("tourBack"));
    m_back->setProperty("role", "secondary");
    m_back->setFocusPolicy(Qt::NoFocus);
    connect(m_back, &QPushButton::clicked, this, &Tour::back);
    buttons->addWidget(m_back);
    m_next = new QPushButton;
    m_next->setObjectName(QStringLiteral("tourNext"));
    m_next->setProperty("role", "primary");
    m_next->setFocusPolicy(Qt::NoFocus);
    connect(m_next, &QPushButton::clicked, this, &Tour::next);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);
}

void Tour::start() {
    if (m_steps.empty()) {
        return;
    }
    setGeometry(m_window.rect());
    m_follow.start();
    QWidget::show();
    raise();
    setFocus();
    show(0);
}

void Tour::next() {
    if (m_step + 1 >= stepCount()) {
        hide();
        emit finished(true);
        deleteLater();
        return;
    }
    show(m_step + 1);
}

void Tour::back() {
    if (m_step > 0) {
        show(m_step - 1);
    }
}

void Tour::finish() {
    hide();
    emit finished(m_step + 1 >= stepCount());
    deleteLater();
}

void Tour::show(int step) {
    m_step = step;
    const TourStep& s = m_steps[static_cast<std::size_t>(step)];
    if (s.enter) {
        s.enter();
    }
    m_count->setText(tr("%1 of %2").arg(step + 1).arg(stepCount()));
    m_title->setText(s.title);
    m_text->setText(s.text);
    m_action->setText(s.actionText);
    m_action->setEnabled(true);
    m_action->setVisible(!s.actionText.isEmpty());
    m_back->setVisible(step > 0);
    QString next = tr("Next");
    if (step == 0) {
        next = tr("Show Me");
    } else if (step + 1 == stepCount()) {
        next = tr("Done");
    }
    m_next->setText(next);
    m_skip->setVisible(step + 1 < stepCount());
    place();
    raise();
}

void Tour::place() {
    if (m_step < 0) {
        return;
    }
    setGeometry(m_window.rect());
    // The part it is about, asked again each time: what a page shows can change meanwhile.
    const TourStep& s = m_steps[static_cast<std::size_t>(m_step)];
    QWidget* target = s.target ? s.target() : nullptr;
    if (target != m_target) {
        if (m_target != nullptr) {
            m_target->removeEventFilter(this);
        }
        m_target = target;
        if (m_target != nullptr) {
            m_target->installEventFilter(this); // followed when it moves or changes size
        }
    }
    if (m_target != nullptr) {
        // Its page may only just have been shown: laid out now, so the spotlight is in place.
        for (const QWidget* widget = m_target; widget != nullptr && widget != &m_window;
             widget = widget->parentWidget()) {
            if (widget->layout() != nullptr) {
                widget->layout()->activate();
            }
        }
    }
    m_spot = {};
    m_targetRect = {};
    if (m_target != nullptr && m_target->isVisible()) {
        const QPoint topLeft = m_target->mapTo(&m_window, QPoint(0, 0));
        m_targetRect = QRect(topLeft, m_target->size());
        m_spot = m_targetRect.adjusted(-kPad, -kPad, kPad, kPad)
                     .intersected(rect().adjusted(4, 4, -4, -4));
    }
    // Tall enough for the wrapped text, measured in the style sheet's font.
    m_card->ensurePolished();
    for (const QLabel* label : {m_count, m_title, m_text}) {
        label->ensurePolished();
    }
    m_card->resize(kCardWidth, m_card->layout()->heightForWidth(kCardWidth));
    const QSize card = m_card->size();
    const QRect room = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    QPoint at;
    if (m_spot.isEmpty()) {
        at = rect().center() - QPoint(card.width() / 2, card.height() / 2);
    } else if (m_spot.right() + kGap + card.width() <= room.right()) { // right of it
        at = QPoint(m_spot.right() + kGap, m_spot.top());
    } else if (m_spot.bottom() + kGap + card.height() <= room.bottom()) { // under it
        at = QPoint(m_spot.left(), m_spot.bottom() + kGap);
    } else if (m_spot.left() - kGap - card.width() >= room.left()) { // left of it
        at = QPoint(m_spot.left() - kGap - card.width(), m_spot.top());
    } else { // over it, inside
        at = QPoint(m_spot.right() - card.width() - kGap, m_spot.bottom() - card.height() - kGap);
    }
    at.setX(std::clamp(at.x(), room.left(), std::max(room.left(), room.right() - card.width())));
    at.setY(std::clamp(at.y(), room.top(), std::max(room.top(), room.bottom() - card.height())));
    m_card->move(at);
    update();
}

void Tour::follow() {
    if (m_step < 0 || !isVisible()) {
        return;
    }
    const TourStep& s = m_steps[static_cast<std::size_t>(m_step)];
    QWidget* target = s.target ? s.target() : nullptr;
    QRect rect;
    if (target != nullptr && target->isVisible()) {
        rect = QRect(target->mapTo(&m_window, QPoint(0, 0)), target->size());
    }
    if (target != m_target || rect != m_targetRect) {
        place();
    }
}

QRect Tour::cardRect() const {
    return m_card->geometry();
}

bool Tour::event(QEvent* event) {
    // While it is up the window's shortcuts (← →, T, Space) don't act under it: their keys
    // come here.
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = dynamic_cast<QKeyEvent*>(event);
        if (key != nullptr && (key->modifiers() & Qt::ControlModifier) == 0) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

bool Tour::eventFilter(QObject* watched, QEvent* event) {
    const bool moved = event->type() == QEvent::Resize || event->type() == QEvent::Move ||
                       event->type() == QEvent::Show || event->type() == QEvent::Hide;
    if ((watched == &m_window || watched == m_target) && moved && isVisible()) {
        place();
    }
    return QWidget::eventFilter(watched, event);
}

void Tour::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Escape:
        finish();
        return;
    case Qt::Key_Right:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        next();
        return;
    case Qt::Key_Left:
        back();
        return;
    default:
        event->accept(); // nothing else reaches the window
    }
}

void Tour::mousePressEvent(QMouseEvent* event) {
    event->accept(); // the window under it waits
}

void Tour::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& c = m_style.colors();
    // Everything dimmed but the spotlight.
    QPainterPath dim;
    dim.addRect(QRectF(rect()));
    if (!m_spot.isEmpty()) {
        QPainterPath hole;
        hole.addRoundedRect(QRectF(m_spot), kRadius, kRadius);
        dim = dim.subtracted(hole);
    }
    painter.fillPath(dim, QColor(0, 0, 0, m_style.isDark() ? 150 : 110));
    if (!m_spot.isEmpty()) {
        painter.setPen(QPen(c.accent, 2));
        painter.drawRoundedRect(QRectF(m_spot).adjusted(1, 1, -1, -1), kRadius, kRadius);
    }
    // The card.
    const QRectF card = QRectF(m_card->geometry()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(c.border, 1));
    painter.setBrush(c.surface);
    painter.drawRoundedRect(card, 12, 12);
}

} // namespace kiebitz::app
