#pragma once

#include <QTimer>
#include <QWidget>

#include "core/json.h"
#include "gui/reader/pdf_view.h"

class QActionGroup;
class QLabel;
class QLineEdit;
class QListWidget;
class QSplitter;
class QStackedWidget;
class QToolBar;
class QToolButton;
class QTreeWidget;

namespace fp::gui {

class AnnotationSidebar;
class NoteEditor;

// A tab showing one PDF attachment with its annotations, plus the parent
// item's notes in a context pane.
class ReaderTab : public QWidget {
    Q_OBJECT
public:
    ReaderTab(int64_t libraryId, const QString& attachmentKey, QWidget* parent = nullptr);
    ~ReaderTab() override;

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

private:
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
    QListWidget* notesList_;
    NoteEditor* noteEditor_;
    QLineEdit* pageEdit_;
    QLabel* pageCount_;
    QLabel* zoomLabel_;
    QLineEdit* find_;
    QLabel* findStatus_;
    QToolButton* colorButton_;
    QActionGroup* tools_;
    QTimer reloadTimer_;
    QTimer positionTimer_;
    bool thumbnailsRequested_ = false;
};

} // namespace fp::gui
