#pragma once

#include <QLayout>
#include <QList>

namespace kiebitz::app {

/// Lays its widgets out left to right, onto a new line when one is full (chips, tags).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6);
    ~FlowLayout() override;
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;
    FlowLayout(FlowLayout&&) = delete;
    FlowLayout& operator=(FlowLayout&&) = delete;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;

private:
    /// Places the items in `rect` (only measures if `test`); returns the height used.
    [[nodiscard]] int arrange(const QRect& rect, bool test) const;

    QList<QLayoutItem*> m_items;
    int m_spacing;
};

} // namespace kiebitz::app
