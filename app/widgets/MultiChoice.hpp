#pragma once

#include <QPushButton>
#include <QStringList>

#include <utility>
#include <vector>

class QMenu;

namespace kiebitz::app {

/// A button that picks any number of choices from a list that stays open while you tick
/// (a profile's "Games from": over the board, Caissabase, Lichess's broadcasts, accounts). The
/// first item, `allText`, is all of them; it is ticked when nothing else is.
class MultiChoice : public QPushButton {
    Q_OBJECT

public:
    explicit MultiChoice(QString allText, QWidget* parent = nullptr);

    /// The choices: their values and texts, in this order.
    void setChoices(std::vector<std::pair<QString, QString>> choices);
    /// The values chosen (empty: all). Unknown values are left out.
    void setSelected(const QStringList& values);
    /// The values chosen, in the choices' order; empty: all.
    [[nodiscard]] QStringList selected() const { return m_selected; }

signals:
    /// The choice changed (by the user).
    void changed();

private:
    void fillMenu();
    void showText();

    QString m_allText;
    std::vector<std::pair<QString, QString>> m_choices;
    QStringList m_selected;
    QMenu* m_menu = nullptr;
};

} // namespace kiebitz::app
