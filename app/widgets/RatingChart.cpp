#include "RatingChart.hpp"

#include "style/AppStyle.hpp"

#include <QDate>
#include <QFontMetricsF>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kiebitz::app {
namespace {

constexpr double kLeft = 44;   ///< room for the ratings
constexpr double kBottom = 22; ///< room for the years

/// Months since year 0: an even time axis.
int monthIndex(int yyyymm) {
    return ((yyyymm / 100) * 12) + std::max(yyyymm % 100, 1) - 1;
}

} // namespace

RatingChart::RatingChart(const AppStyle& style, QWidget* parent) : QWidget(parent), m_style(style) {
    setObjectName(QStringLiteral("ratingChart"));
    setMinimumHeight(140);
    setMouseTracking(true);
}

void RatingChart::setRatings(std::vector<std::pair<int, int>> ratings) {
    m_ratings = std::move(ratings);
    if (!m_ratings.empty()) {
        const auto [low, high] =
            std::minmax_element(m_ratings.begin(), m_ratings.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; });
        // Steps of 50 around them, at least 100 apart.
        m_low = (low->second / 50) * 50;
        m_high = ((high->second + 49) / 50) * 50;
        if (m_high - m_low < 100) {
            m_high = m_low + 100;
        }
    }
    update();
}

QRectF RatingChart::plot() const {
    return QRectF(rect()).adjusted(kLeft, 8, -8, -kBottom);
}

QPointF RatingChart::pointAt(std::size_t index) const {
    const QRectF area = plot();
    const int first = monthIndex(m_ratings.front().first);
    const int span = std::max(monthIndex(m_ratings.back().first) - first, 1);
    const double x =
        m_ratings.size() == 1
            ? area.center().x()
            : area.left() + (area.width() * (monthIndex(m_ratings[index].first) - first) / span);
    const double y = area.bottom() - (area.height() * (m_ratings[index].second - m_low) /
                                      static_cast<double>(m_high - m_low));
    return {x, y};
}

double RatingChart::yearLabelWidth() const {
    QFont small = font();
    small.setPixelSize(kLabelPixels);
    return QFontMetricsF(small).horizontalAdvance(QStringLiteral("0000")) + 8;
}

std::vector<std::pair<double, int>> RatingChart::yearLabels() const {
    std::vector<std::pair<double, int>> labels;
    if (m_ratings.empty()) {
        return labels;
    }
    // At most about eight years, each where it starts, kept inside the chart at its ends.
    const QRectF area = plot();
    const double labelWidth = yearLabelWidth();
    const int firstYear = m_ratings.front().first / 100;
    const int lastYear = m_ratings.back().first / 100;
    const int every = std::max(1, (lastYear - firstYear + 1) / 8);
    const int first = monthIndex(m_ratings.front().first);
    const int span = std::max(monthIndex(m_ratings.back().first) - first, 1);
    for (int year = firstYear; year <= lastYear; year += every) {
        const int at = std::max(monthIndex((year * 100) + 1), first);
        const double x = m_ratings.size() == 1 ? area.center().x()
                                               : area.left() + (area.width() * (at - first) / span);
        labels.emplace_back(std::clamp(x - (labelWidth / 2), 0.0, width() - labelWidth), year);
    }
    // Never overlapping: the first year, when the line starts late in it, gives way to the
    // next one; after that, a label too close to the one before is left out.
    if (labels.size() > 1 && labels[1].first < labels[0].first + labelWidth) {
        labels.erase(labels.begin());
    }
    std::vector<std::pair<double, int>> shown;
    for (const auto& label : labels) {
        if (shown.empty() || label.first >= shown.back().first + labelWidth) {
            shown.push_back(label);
        }
    }
    return shown;
}

void RatingChart::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& c = m_style.colors();
    const QRectF area = plot();
    QFont font = painter.font();
    font.setPixelSize(kLabelPixels);
    painter.setFont(font);
    if (m_ratings.empty()) {
        painter.setPen(c.muted);
        painter.drawText(area, Qt::AlignCenter, tr("No rated games"));
        return;
    }
    // The grid: four steps between the lowest and highest round number.
    for (int k = 0; k <= 4; ++k) {
        const double y = area.bottom() - (area.height() * k / 4.0);
        painter.setPen(QPen(c.border, 1));
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        painter.setPen(c.muted);
        painter.drawText(QRectF(0, y - 7, kLeft - 8, 14), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(m_low + ((m_high - m_low) * k / 4)));
    }
    // The years along the bottom.
    painter.setPen(c.muted);
    const double labelWidth = yearLabelWidth();
    for (const auto& [left, year] : yearLabels()) {
        painter.drawText(QRectF(left, area.bottom() + 4, labelWidth, kBottom - 4),
                         Qt::AlignHCenter | Qt::AlignTop, QString::number(year));
    }
    // The line, and a dot on the last month.
    QPainterPath line;
    for (std::size_t i = 0; i < m_ratings.size(); ++i) {
        if (i == 0) {
            line.moveTo(pointAt(i));
        } else {
            line.lineTo(pointAt(i));
        }
    }
    painter.setPen(QPen(c.accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
    painter.setPen(Qt::NoPen);
    painter.setBrush(c.accent);
    painter.drawEllipse(pointAt(m_ratings.size() - 1), 3.5, 3.5);
}

void RatingChart::mouseMoveEvent(QMouseEvent* event) {
    if (m_ratings.empty()) {
        return;
    }
    // The month nearest the pointer.
    std::size_t nearest = 0;
    double distance = 1e9;
    for (std::size_t i = 0; i < m_ratings.size(); ++i) {
        const double apart = std::abs(pointAt(i).x() - event->position().x());
        if (apart < distance) {
            distance = apart;
            nearest = i;
        }
    }
    const auto [month, rating] = m_ratings[nearest];
    const QString when = month % 100 == 0 ? QString::number(month / 100)
                                          : QLocale().toString(QDate(month / 100, month % 100, 1),
                                                               QStringLiteral("MMM yyyy"));
    QToolTip::showText(event->globalPosition().toPoint(),
                       QStringLiteral("%1: %2").arg(when).arg(rating), this);
}

} // namespace kiebitz::app
