#pragma once

#include <QTimer>
#include <QWidget>

#include <optional>

#include "core/json.h"
#include "gui/reader/pdf_view.h"

class QActionGroup;
class QLabel;
class QLineEdit;
class QSlider;
class QListWidget;
class QSplitter;
class QStackedWidget;
class QToolBar;
class QToolButton;
class QTreeWidget;

class QUndoStack;
class QTabWidget;

namespace on::gui {

class AnnotationSidebar;
class SearchField;
class InfoEditor;
class NoteEditor;

// A tab showing one PDF attachment with its annotations, plus the parent item's
// Info (fields, tags, related) and notes in a side pane.
class ReaderTab : public QWidget {
    Q_OBJECT
    friend class AnnotationEdit;

public:
    ReaderTab(int64_t libraryId, const QString& attachmentKey, QWidget* parent = nullptr);
    ~ReaderTab() override;

    // Tab title for an attachment: its parent item's title, else its own.
    static QString titleFor(int64_t libraryId, const QString& attachmentKey);

    int64_t libraryId() const { return libraryId_; }
    QString attachmentKey() const { return key_; }
    QString title() const { return title_; }
    PdfView* view() const { return view_; }
    void goToPage(int page);
    void selectAnnotation(const QString& key);
    void focusFind();
    void exportPdf();

signals:
    void titleChanged(const QString& title);
    void showInLibrary(qint64 libraryId, const QString& key);
    void libraryRequested();

protected:
    void changeEvent(QEvent* e) override;
    // Esc in a field (the annotations' search, the page number, Info, a note…) that has no
    // use for it: the keys back on the page ([, ], I, N…)
    void keyPressEvent(QKeyEvent* e) override;

private:
    void updateDarkPagesAction();
    void requestThumbnails();
    void buildToolbar();
    void buildSidebar();
    void buildContextPane();
    void reloadAnnotations();
    void reloadNotes();
    void importEmbeddedAnnotations();
    void createAnnotation(const Json& data);
    void saveAnnotation(const Json& data);
    void deleteAnnotation(const QString& key);
    void addToNote(const QString& html);
    void addNoteFromAnnotations();
    void updatePageLabel(int page);
    void setTool(ReaderTool tool);
    void savePosition();
    void saveViewState();
    // Annotation changes go through the undo stack (Ctrl+Z / Ctrl+Y, 50 steps, per tab).
    void pushAnnotationEdit(std::optional<Json> before, std::optional<Json> after, const QString& text);
    bool forwardUndoToText(bool undo);
    QString citationFor(const Json& annotation) const;

    int64_t libraryId_;
    QString key_;
    QString parentKey_;
    QString title_;
    bool editable_ = true;

    QToolBar* toolbar_;
    PdfView* view_;
    QSplitter* splitter_;
    QStackedWidget* sidebar_;
    AnnotationSidebar* annotations_;
    QListWidget* thumbnails_;
    QTreeWidget* outline_;
    QWidget* contextPane_;
    QTabWidget* contextTabs_;  // Info, Notes
    InfoEditor* info_;
    QListWidget* notesList_;
    NoteEditor* noteEditor_;
    QLineEdit* pageEdit_;
    QLabel* pageCount_;
    QLabel* zoomLabel_;
    SearchField* find_;
    QLabel* findStatus_;
    QToolButton* colorButton_;
    QActionGroup* tools_;
    QTimer reloadTimer_;
    QTimer positionTimer_;  // the synced last page, after page changes
    QTimer stateTimer_;     // the local view state, after scrolling or zooming
    bool restored_ = false;
    QUndoStack* undo_;
    bool thumbnailsRequested_ = false;
    QAction* darkPages_ = nullptr;
    QAction* sideToggle_ = nullptr;  // the panel on the left: annotations, thumbnails, outline
    QAction* paneToggle_ = nullptr;  // the panel on the right: Info and Notes
    QSlider* pageTone_ = nullptr;
};

} // namespace on::gui
