#pragma once

#include <QWidget>

#include <cstdint>

namespace on::gui {

class NoteEditor;

// A note filed under no item, in a tab of its own: opened from the library as a PDF is
// (a double-click, Enter), the note's editor alone, without the panes beside a PDF.
class NoteTab : public QWidget {
    Q_OBJECT
public:
    NoteTab(int64_t libraryId, const QString& noteKey, QWidget* parent = nullptr);

    // A note tab's title: the note's first line, "(empty note)"; empty when the note is
    // gone.
    static QString titleFor(int64_t libraryId, const QString& noteKey);

    int64_t libraryId() const { return libraryId_; }
    QString noteKey() const { return key_; }  // (its own: the editor forgets a deleted note)
    NoteEditor* editor() const { return editor_; }
    void focusEditor();
    void focusFind();

private:
    int64_t libraryId_;
    QString key_;
    NoteEditor* editor_;
};

} // namespace on::gui
