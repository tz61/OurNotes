#pragma once

#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QListWidget;
class QLabel;
class QToolBar;

namespace on::gui {

class WrapToolBar;

class FittedImageHandler;

// QTextEdit for notes: stores pasted/dropped images as embedded note attachments,
// fits images to the width, and edits LaTeX formulas ($…$ and $$…$$).
class NoteTextEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit NoteTextEdit(QWidget* parent = nullptr);
    std::function<QString(const QImage&)> storeImage;  // returns a resource name

    // Starts an empty formula (or one made of the selected text) at the cursor, in place.
    void insertMath(bool display);
    // After loading HTML: formulas on the text baseline, readable text colours.
    void prepareDocument();
    // Renders formulas whose source is being edited (all of them, or those the
    // cursor has left).
    void commitMathEdits(bool all);
    // True while the editor adjusts the document itself (not a user edit).
    bool isInternalEdit() const { return internal_ > 0; }

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    struct MathRun {
        int start = 0, end = 0;
        bool display = false;
    };
    bool convertTypedMath();
    void commitAs(QTextCursor selection, const QString& latex, bool display);
    QTextCursor mathAt(const QPoint& viewportPos);  // selects the formula under the point
    void editMathInPlace(QTextCursor selection);
    bool editMathInDialog(QTextCursor selection);
    // `original`: the formula being changed (empty for a new one).
    void startMathEdit(QTextCursor cursor, const QString& latex, bool display, const QString& original = {});
    bool finishMathSession(const QString& latex, QTextCursor* after);
    QTextCharFormat sourceFormat(bool display, QTextCharFormat base) const;
    std::vector<MathRun> mathRuns() const;
    std::optional<MathRun> runAtCursor() const;
    void updateMathPreview();
    void leaveMath(const MathRun& run, bool nextLine);
    QTextCharFormat outerFormat(const QTextBlock& block, int formulaPosInBlock) const;
    void adaptColors();

    FittedImageHandler* images_ = nullptr;
    QLabel* preview_ = nullptr;
    QPoint pressPos_;
    int internal_ = 0;
    bool committing_ = false;
    bool editingMath_ = false;  // the cursor was inside a formula's source
    // A formula being edited is one step in the undo history: when it is rendered, the
    // steps since it was opened are undone and the result is applied as one edit.
    struct MathSession {
        int undoSteps = -1;     // undo steps before it was opened; -1: no session
        int start = 0, length = 0;  // what it replaces, in the document of that time
        bool display = false;
        QString original;
    } session_;
};

// Rich-text editor for a note item. Changes are saved automatically shortly
// after typing stops; untouched notes are never rewritten.
class NoteEditor : public QWidget {
    Q_OBJECT
public:
    explicit NoteEditor(QWidget* parent = nullptr);
    ~NoteEditor() override;

    void setNote(int64_t libraryId, const QString& key);
    void clear();
    QString key() const { return key_; }
    int64_t libraryId() const { return libraryId_; }
    void flush();
    void appendHtml(const QString& html);
    void focusEditor();

signals:
    void saved(const QString& key);

private:
    // force = false: a change notification, which is skipped when it is this editor's
    // own save (reloading would lose the undo history)
    void load(bool force = true);
    void save();
    void setHeading(int level);
    void toggleList(bool ordered);
    void toggleQuote();
    void insertLink();
    void setTextColor(const QColor& c, bool background);
    void updateToolbar();
    QString storeImage(const QImage& image);

    NoteTextEdit* edit_;
    WrapToolBar* toolbar_;
    QComboBox* blockStyle_;
    QAction* boldAct_;
    QAction* italicAct_;
    QAction* underlineAct_;
    QAction* strikeAct_;
    QTimer saveTimer_;
    int64_t libraryId_ = 0;
    QString key_;
    QString lastSaved_;
    QString lastSerialized_;  // the stored note as this editor writes it
    bool loading_ = false;
};

// Lets the notes listed in `list` (keys in Qt::UserRole) be moved to the trash, from
// the context menu or with Delete, after asking.
void enableNoteTrash(QListWidget* list, std::function<int64_t()> libraryId);

} // namespace on::gui
