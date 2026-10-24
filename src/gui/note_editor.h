#pragma once

#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QFrame;
class QListWidget;
class QLabel;
class QToolBar;
class QSplitter;
class QToolButton;
class QTextDocument;

namespace on::gui {

class SearchField;
class WrapToolBar;

class FittedImageHandler;

// QTextEdit for notes: stores pasted/dropped images as embedded note attachments,
// fits images to the width, and edits LaTeX formulas ($…$ and $$…$$).
class NoteTextEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit NoteTextEdit(QWidget* parent = nullptr);
    std::function<QString(const QImage&)> storeImage;  // returns a resource name
    // Ctrl+wheel, Ctrl+= / Ctrl+- / Ctrl+0: +1 larger text, -1 smaller, 0 the default
    std::function<void(int)> zoomBy;
    // Markdown typed becomes formatting (# heading, - list, **bold**, `code`, [a](url)),
    // and so does Markdown pasted as plain text; Ctrl+Shift+V pastes text as it is.
    void setMarkdownShortcuts(bool on) { markdown_ = on; }
    // Formats the cursor's paragraph for a typed "# ", "- ", "1. " or "> ": kind is
    // "heading1".."heading3", "bullet", "numbered" or "quote".
    std::function<void(const QString& kind)> markdownBlock;
    // Ctrl+F in the note (not the window's search, or a PDF's find), when set
    std::function<void()> findRequested;

    // Starts an empty formula (or one made of the selected text) at the cursor, in place.
    void insertMath(bool display);
    // After loading HTML: formulas on the text baseline, readable text colours.
    void prepareDocument();
    // Renders formulas whose source is being edited (all of them, or those the
    // cursor has left).
    void commitMathEdits(bool all);
    // True while the editor adjusts the document itself (not a user edit).
    bool isInternalEdit() const { return internal_ > 0; }

    // Tables: one with a header row and an empty row, `columns` wide, at `at` (after its
    // line, or in it when it is empty); and what the context menu does to the table at `at`.
    void insertTable(const QTextCursor& at, int columns = 2);
    enum class TableEdit { RowAbove, RowBelow, ColumnLeft, ColumnRight, DeleteRow, DeleteColumn, DeleteTable };
    void editTable(const QTextCursor& at, TableEdit edit);

    // Pictures (note_pictures.cpp), as in Feishu or Slack: a click on one picks it (an
    // outline, corners to drag, no text cursor), with buttons above it: sizes, a caption, how
    // it is aligned, beside the pictures above or on its own line, open, delete; a click
    // beside one puts the cursor there. Pictures alone on a line are a row, all as high; one
    // made larger or smaller makes the row so.
    std::function<void(const QString& name)> openPicture;  // a picture's resource name
    // A picture of another note, pasted from it, by its resource name ("attachment:KEY"):
    // this note keeps a copy of its own (storeImage).
    std::function<QImage(const QString& name)> loadPicture;
    int pickedPicture() const;  // where the picture picked is, or -1
    void pickPicture(int pos);
    QRectF pictureRect(int pos) const;  // where it is drawn, in the viewport
    enum class PictureSize { Small, Medium, Large, FitWidth, Original };
    void resizePicture(PictureSize size);  // the picked picture, and its row
    void addCaption();
    void alignPictures(Qt::Alignment align);
    bool joinPreviousRow();  // the picked picture to the end of the row above
    bool pictureOwnLine();   // the picked picture out of its row (or text), on a line of its own
    // backwards: the cursor where it was, before it; keepCaption: one it had alone stays, as text
    void deletePicture(bool backwards = false, bool keepCaption = false);
    bool movePicture(int from, int to);  // as dragged: from where it is to before `to`
    // Where the cursor is: into a line of pictures there (right after one picked), else on a
    // line of their own (one row); the last one picked.
    void insertPictures(const QList<QImage>& images);

protected:
    QMimeData* createMimeDataFromSelection() const override;
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
    void wheelEvent(QWheelEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;  // (text at pictures: on a line of its own)

private:
    int zoomKey(const QKeyEvent* e) const;  // what zoomBy gets, or -2 for other keys
    int formulaKey(const QKeyEvent* e) const;
    bool convertTypedMarkdown(const QString& typed);
    bool convertTypedTable();       // Enter after a table's header and delimiter rows
    bool tableKey(QKeyEvent* e);    // Tab, Shift+Tab, Enter in a table
    // Text as rich text in `out`, Markdown or plain; its formulas ($…$, $$…$$) as formulas.
    void textDocument(const QString& text, bool markdown, QTextDocument* out) const;
    bool markdown_ = false;
    int wheelZoom_ = 0;  // Ctrl+wheel turned less than a notch
    struct MathRun {
        int start = 0, end = 0;
        bool display = false;
    };
    bool convertTypedMath();
    // ownLine: a display formula that is its line, centred, the cursor on the next one
    void commitAs(QTextCursor selection, const QString& latex, bool display, bool ownLine = true);
    bool reopenEmptyMath();
    void revealHeading(QTextCursor cursor, int level);
    void finishHeading();
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

    // pictures (note_pictures.cpp)
    void setUpPictures();
    void unpickPicture();
    // The picture at a point; edge: -1 or 1 on its left or right edge (where a click puts the
    // cursor beside it), else 0.
    int pictureAt(const QPoint& viewportPos, int* edge = nullptr) const;
    int pictureCornerAt(const QPoint& viewportPos) const;  // of the picked picture: 0..3, or -1
    bool pictureKey(QKeyEvent* e);    // a key for the picked picture (true: used)
    bool rowKey(QKeyEvent* e);        // a key with the cursor in a line of pictures
    // Where text typed (or pasted) at pictures goes: on a line of its own under them (above
    // them, from the start of their line); after one picked in a line of text. A format set
    // for what is typed next (bold, say) goes with it. True when it made a line (a step to
    // undo, which what is then written joins).
    bool caretForText();
    void typeAside();  // caretForText for a key typed: its line and it one step (typingStep_)
    bool captionKey(QKeyEvent* e);    // Enter, Backspace, Delete next to pictures and captions
    // The cursor in a line of pictures: drawn here, as high as text (Qt's is as high as they
    // are), blinking; Qt's hidden then, and while one is picked or something dragged over.
    void updateCaret();
    QRect rowCaretRect() const;
    QRect staleCaret() const;  // where the drawn cursor was, now (the note scrolled since)
    void finishComposing();    // an input method's text, before a click Qt does not see
    void endComposing();       // (see composeTo_)
    // Pictures of another note in what is pasted (not among this note's): this note's own
    // copies of them, else they stayed that note's, and were shown blank here.
    void adoptPictures(QTextDocument* pasted, const QMimeData* source);
    bool pictureMousePress(QMouseEvent* e);
    bool pictureMouseMove(QMouseEvent* e);
    bool pictureMouseRelease(QMouseEvent* e);
    void startPictureDrag();
    void copyPicture(bool cut);
    void setPictureHeight(int pos, qreal height);  // its own, or its row's
    QTextCursor newLineAfter(const QTextBlock& row);   // under a line of pictures (and its caption)
    QTextCursor newLineBefore(const QTextBlock& row);  // above it
    QTextCursor emptyLineAt(const QTextBlock& row, bool above);  // an empty one there, else a new one
    void placePictures(const QList<QTextImageFormat>& pictures);  // (see insertPictures)
    bool picturesOnly(const QMimeData* source) const;  // put as pictures (insertFromMimeData)
    void placePictureBar();
    QTextCursor picked_;  // the picture picked, selected
    QFrame* pictureBar_ = nullptr;
    QTimer pictureBarTimer_;
    struct {
        int corner = -1;  // 0 top left, 1 top right, 2 bottom left, 3 bottom right
        QPoint start;
        QRectF rect;
        qreal height = 0;
        int at = -1;  // the picture's place (its line laid out again at the end)
    } resizing_;
    int pressedPicture_ = -1;
    QPoint pressedAt_;
    QTextCursor dragged_;  // the picture being dragged
    int caretWidth_ = 1;
    int picking_ = 0;
    bool rowCaret_ = false;  // the cursor in a line of pictures, drawn by paintEvent
    bool caretShown_ = true;  // (blinking)
    QRect drawnCaret_;
    QPoint caretScroll_;  // (the scroll bars' values when it was drawn)
    QTextCursor typingStep_;  // text typed at pictures: its new line and it, one step to undo
    // an input method's text at pictures: where the cursor was moved to for it, the line
    // made for it (gone again if nothing is written, while it is the last step to undo:
    // composeSteps_), how far it was moved in its line (what it replaces is that far back)
    QTextCursor composeTo_, composeLine_;
    int composeSteps_ = -1, composeShift_ = 0;
    QTimer caretBlink_;
    int dropAt_ = -1;  // where what is dragged over the note would go
    // (a line of its own made there: under the block at dropBlock_, or above it: 1, -1)
    int dropLine_ = 0, dropBlock_ = -1;
    bool dropPictures_ = false;  // (what is dragged is put as pictures)

    FittedImageHandler* images_ = nullptr;
    QLabel* preview_ = nullptr;
    QPoint pressPos_;
    int internal_ = 0;
    bool committing_ = false;
    bool editingMath_ = false;  // the cursor was inside a formula's source
    bool pairTyped_ = false;    // the formula being typed was opened with "$$" in a line
    bool swallowDollar_ = false;  // a formula was just closed with "$": a second one is not text
    QTextCursor revealed_;      // the heading shown as "## …" (see revealHeading)
    // A formula being edited is one step in the undo history: when it is rendered, the
    // steps since it was opened are undone and the result is applied as one edit.
    struct MathSession {
        int undoSteps = -1;     // undo steps before it was opened; -1: no session
        int start = 0, length = 0;  // what it replaces, in the document of that time
        bool display = false;
        QString original;
        bool joinLine = false;  // a line made for it at pictures: its step joined to that one
    } session_;
};

// Rich-text editor for a note item. Changes are saved automatically shortly
// after typing stops; untouched notes are never rewritten.
class NoteEditor : public QWidget {
    Q_OBJECT
public:
    explicit NoteEditor(QWidget* parent = nullptr);
    ~NoteEditor() override;
    // The same with the find bar open or closed: a change of the window's minimum moved
    // the window's edge.
    QSize minimumSizeHint() const override;

    void setNote(int64_t libraryId, const QString& key);
    void clear();
    QString key() const { return key_; }
    int64_t libraryId() const { return libraryId_; }
    void flush();
    void appendHtml(const QString& html);
    void focusEditor();
    // Text size, in percent of the interface's (the stored note is not changed); it
    // starts at the default from the preferences.
    int zoom() const { return zoom_; }
    void setZoom(int percent);
    static int defaultZoom();  // /notes/zoom
    // Find in the note: a bar at the bottom of the note, opened with Ctrl+F there and
    // closed with Esc, a search field like the others (history, Tab, Aa); the hits are
    // marked, the current one more, Enter goes to the next one and Shift+Enter back.
    void openFind();

signals:
    void saved(const QString& key);

protected:
    void keyPressEvent(QKeyEvent* e) override;  // Esc in the note: the find bar closed first
    void hideEvent(QHideEvent* e) override;

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
    void zoomBy(int steps);
    QString storeImage(const QImage& image);
    void buildFindBar();  // (see openFind)
    void closeFind(bool focusNote);
    void updateHits(bool fromCursor);
    void findAgain(bool backwards);
    void markHits();

    NoteTextEdit* edit_;
    QWidget* findBar_ = nullptr;
    SearchField* find_ = nullptr;
    QLabel* findStatus_ = nullptr;
    std::vector<QTextCursor> hits_;
    int hit_ = -1;
    WrapToolBar* toolbar_;
    QComboBox* blockStyle_;
    QAction* boldAct_;
    QAction* italicAct_;
    QAction* underlineAct_;
    QAction* strikeAct_;
    QToolButton* zoomButton_;  // shows the text size, resets it
    QFont baseFont_;
    int zoom_ = 100;
    int defaultZoom_ = 100;
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

// The list of notes over the open note: one height for the list in all of them (the
// library's Notes tab and every PDF's), kept in the settings; dragging one moves all.
void shareNotesSplit(QSplitter* split);

} // namespace on::gui
