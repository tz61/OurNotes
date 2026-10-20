#pragma once

#include <QLineEdit>
#include <QStringList>

class QFrame;
class QLabel;
class QListWidget;

namespace on::gui {

// A search field (the library's, a PDF's find) with a history of searches as in a shell
// (oh-my-zsh's):
// - Up and Down go through the earlier searches that start with what was typed (all of
//   them when nothing was, or when what is in the field was not typed: the search just
//   made, one taken from the history), the latest first, and back down to it.
// - Tab completes from them: the one that matches, whole, or as far as all that match
//   agree. When that adds nothing, Tab lists them, five at a time with "7/51" in the
//   corner when there are more, the first highlighted and in the field; more Tabs
//   (Shift+Tab back) or the arrow keys go through the list, PgDn and PgUp five at a
//   time, Ctrl+Backspace forgets the highlighted one, Enter or a click takes one, Esc
//   goes back to what was typed. With text in the field Tab stays there (it is for
//   completing); in an empty one it moves on.
// - Esc otherwise: escapePressed (the library clears the search and the selection, a PDF
//   its find and the hits).
// - Shift+Enter: shiftReturnPressed (a PDF finds backwards), not returnPressed.
// A search is kept when Enter is pressed or the field is left with it, the last 1000 in
// the settings under historyKey (the latest last).
class SearchField : public QLineEdit {
    Q_OBJECT
public:
    explicit SearchField(QWidget* parent = nullptr, const QString& historyKey = "/ui/searchHistory");
    QStringList history() const { return history_; }
    void remember(const QString& text);
    bool isListShown() const;
    QStringList listed() const;   // the searches in the list, all of them
    int listedCurrent() const;    // the highlighted one, -1 for none
    QString listedCount() const;  // what the corner shows ("7/51"; empty for five or fewer)

signals:
    void escapePressed();
    void shiftReturnPressed();

protected:
    bool event(QEvent* e) override;  // Tab and Shift+Tab, before they move the focus
    void keyPressEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    QStringList matching(const QString& prefix) const;  // latest first, each once
    bool complete();  // false: nothing to complete
    void showList(const QStringList& searches);
    void hideList(bool backToTyped);
    void moveInList(int step, bool wrap = true);
    void forgetCurrent();  // Ctrl+Backspace in the list
    void saveHistory();
    void browse(int step);  // Up (+1, older) or Down (-1)
    void fill(const QString& text);  // in the field, as if typed there

    QString historyKey_;
    QStringList history_;
    QFrame* popup_ = nullptr;  // the list and its count
    QListWidget* list_ = nullptr;
    QLabel* count_ = nullptr;
    bool typedByUser_ = false;  // what is in the field was typed there (not a search kept or taken)
    QString typed_;      // what was typed, before Up or the list
    int browsing_ = -1;  // where in browsed_ Up and Down are; -1: at what was typed
    QStringList browsed_;
};

} // namespace on::gui
