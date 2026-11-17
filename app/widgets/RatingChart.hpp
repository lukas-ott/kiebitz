#pragma once

#include <QWidget>

#include <utility>
#include <vector>

namespace kiebitz::app {

class AppStyle;

/// A player's rating over time as a line: a point per month (yyyymm), a light grid with the
/// ratings on the left and the years under it. Hovering says the month and the rating.
class RatingChart : public QWidget {
    Q_OBJECT

public:
    explicit RatingChart(const AppStyle& style, QWidget* parent = nullptr);
    /// (yyyymm, rating), oldest first.
    void setRatings(std::vector<std::pair<int, int>> ratings);
    [[nodiscard]] const std::vector<std::pair<int, int>>& ratings() const { return m_ratings; }
    /// The years under the line, as drawn: (left edge, year), never overlapping.
    [[nodiscard]] std::vector<std::pair<double, int>> yearLabels() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    static constexpr int kLabelPixels = 10;
    [[nodiscard]] double yearLabelWidth() const;
    [[nodiscard]] QRectF plot() const;
    [[nodiscard]] QPointF pointAt(std::size_t index) const;

    const AppStyle& m_style;
    std::vector<std::pair<int, int>> m_ratings;
    int m_low = 0; ///< the scale: round numbers around the ratings
    int m_high = 0;
};

} // namespace kiebitz::app
