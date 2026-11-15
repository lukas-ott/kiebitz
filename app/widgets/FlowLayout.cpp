#include "FlowLayout.hpp"

#include <QWidget>

#include <algorithm>

namespace kiebitz::app {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), m_spacing(spacing) {
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
    qDeleteAll(m_items);
}

void FlowLayout::addItem(QLayoutItem* item) {
    m_items.append(item);
}

int FlowLayout::count() const {
    return static_cast<int>(m_items.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const {
    return m_items.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index) {
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return arrange(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    (void)arrange(rect, false);
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : m_items) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

int FlowLayout::arrange(const QRect& rect, bool test) const {
    const QMargins margins = contentsMargins();
    const QRect area =
        rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    int x = area.x();
    int y = area.y();
    int lineHeight = 0;
    for (QLayoutItem* item : m_items) {
        if (item->widget() != nullptr && item->widget()->isHidden()) {
            continue; // hidden on purpose (one about to show isn't: it gets its place now)
        }
        const QSize hint = item->sizeHint();
        if (x + hint.width() > area.right() + 1 && lineHeight > 0) {
            x = area.x();
            y += lineHeight + m_spacing;
            lineHeight = 0;
        }
        if (!test) {
            item->setGeometry(QRect(QPoint(x, y), hint));
        }
        x += hint.width() + m_spacing;
        lineHeight = std::max(lineHeight, hint.height());
    }
    return y + lineHeight - rect.y() + margins.bottom();
}

} // namespace kiebitz::app
