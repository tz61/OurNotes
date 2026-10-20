#pragma once

#include <QLineEdit>
#include <QStringList>

class QListWidget;

namespace on::gui {

// The library's search field, with a history of searches as in a shell (oh-my-zsh's):
// - Up and Down go through the earlier searches that start with what was typed (all of
//   them when nothing was), the latest first, and back down to what was typed.
// - Tab completes from them: the one that matches, whole, or as far as all that match
//   agree. When that adds nothing, Tab lists them, the first highlighted and in the field;
//   more Tabs (Shift+Tab back) or the arrow keys go through the list, Enter or a click
//   takes one, Esc goes back to what was typed. With nothing to complete, Tab moves on.
// - Esc otherwise: escapePressed (the library clears the search and the selection).
// A search is kept when Enter is pressed or the field is left with it, in the settings
// ("/ui/searchHistory", the latest last).
class SearchField : public QLineEdit {
    Q_OBJECT
public:
    explicit SearchField(QWidget* parent = nullptr);
    QStringList history() const { return history_; }
    void remember(const QString& text);
    bool isListShown() const;
    QStringList listed() const;  // the searches in the list, as shown
    int listedCurrent() const;   // the highlighted one, -1 for none

signals:
    void escapePressed();

protected:
    bool event(QEvent* e) override;  // Tab and Shift+Tab, before they move the focus
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    QStringList matching(const QString& prefix) const;  // latest first, each once
    bool complete();  // false: nothing to complete
    void showList(const QStringList& searches);
    void hideList(bool backToTyped);
    void moveInList(int step);
    void browse(int step);  // Up (+1, older) or Down (-1)
    void fill(const QString& text);  // in the field, as if typed there

    QStringList history_;
    QListWidget* list_ = nullptr;
    QString typed_;      // what was typed, before Up or the list
    int browsing_ = -1;  // where in browsed_ Up and Down are; -1: at what was typed
    QStringList browsed_;
};

} // namespace on::gui
