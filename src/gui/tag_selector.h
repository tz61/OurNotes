#pragma once

#include <QWidget>

class QLineEdit;
class QListWidget;
class QToolButton;

namespace fp::gui {

// Tag filter under the collection tree: coloured tags first, then all tags.
// Selecting tags filters the item list to items having all of them.
class TagSelector : public QWidget {
    Q_OBJECT
public:
    explicit TagSelector(QWidget* parent = nullptr);

    void setLibrary(int64_t libraryId);
    QStringList selectedTags() const;

signals:
    void selectionChanged(const QStringList& tags);

private:
    void reload();
    void showMenu(const QPoint& pos);

    int64_t libraryId_ = 0;
    QListWidget* list_;
    QLineEdit* filter_;
    QToolButton* deselect_;
};

} // namespace fp::gui
