#include "MultiChoice.hpp"

#include <QAction>
#include <QMenu>
#include <QMouseEvent>

#include <algorithm>

namespace kiebitz::app {
namespace {

/// A menu that stays open when a tickable item is clicked: several can be ticked in a row.
class StayingMenu : public QMenu {
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (QAction* action = activeAction();
            action != nullptr && action->isCheckable() && action->isEnabled()) {
            action->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
};

} // namespace

MultiChoice::MultiChoice(QString allText, QWidget* parent)
    : QPushButton(parent), m_allText(std::move(allText)), m_menu(new StayingMenu(this)) {
    setProperty("role", "secondary");
    setFocusPolicy(Qt::NoFocus);
    setMenu(m_menu);
    showText();
}

void MultiChoice::setChoices(std::vector<std::pair<QString, QString>> choices) {
    m_choices = std::move(choices);
    fillMenu();
    setSelected(m_selected); // those still there
}

void MultiChoice::setSelected(const QStringList& values) {
    m_selected.clear();
    for (const auto& [value, text] : m_choices) {
        if (values.contains(value)) {
            m_selected << value;
        }
    }
    if (m_selected.size() == static_cast<qsizetype>(m_choices.size())) {
        m_selected.clear(); // every one: all
    }
    // The ticks (the menu itself stays: one of its items may be what is being clicked).
    for (QAction* action : m_menu->actions()) {
        if (action->objectName() == u"choice:all") {
            action->setChecked(m_selected.isEmpty());
        } else if (action->objectName().startsWith(u"choice:")) {
            action->setChecked(m_selected.contains(action->objectName().mid(7)));
        }
    }
    showText();
}

void MultiChoice::fillMenu() {
    m_menu->clear();
    QAction* all = m_menu->addAction(m_allText);
    all->setObjectName(QStringLiteral("choice:all"));
    all->setCheckable(true);
    all->setChecked(m_selected.isEmpty());
    connect(all, &QAction::triggered, this, [this] {
        setSelected({});
        emit changed();
    });
    m_menu->addSeparator();
    for (const auto& [value, text] : m_choices) {
        QAction* action = m_menu->addAction(text);
        action->setObjectName(QStringLiteral("choice:") + value);
        action->setCheckable(true);
        action->setChecked(m_selected.contains(value));
        connect(action, &QAction::triggered, this, [this, toggled = value] {
            QStringList chosen = m_selected;
            if (chosen.contains(toggled)) {
                chosen.removeAll(toggled);
            } else {
                chosen << toggled;
            }
            setSelected(chosen);
            emit changed();
        });
    }
}

void MultiChoice::showText() {
    if (m_selected.isEmpty()) {
        setText(m_allText);
        return;
    }
    QStringList texts;
    for (const auto& [value, text] : m_choices) {
        if (m_selected.contains(value)) {
            texts << text;
        }
    }
    // Two read well together; more are counted.
    setText(texts.size() <= 2 ? texts.join(QStringLiteral(" + "))
                              : tr("%1 sources").arg(texts.size()));
    setToolTip(texts.join(u'\n'));
}

} // namespace kiebitz::app
