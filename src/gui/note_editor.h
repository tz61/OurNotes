#pragma once

#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include <functional>

class QComboBox;
class QToolBar;

namespace fp::gui {

// QTextEdit that stores pasted/dropped images as embedded note attachments.
class NoteTextEdit : public QTextEdit {
    Q_OBJECT
public:
    using QTextEdit::QTextEdit;
    std::function<QString(const QImage&)> storeImage;  // returns a resource name

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
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
    void load();
    void save();
    void setHeading(int level);
    void toggleList(bool ordered);
    void toggleQuote();
    void insertLink();
    void setTextColor(const QColor& c, bool background);
    void updateToolbar();
    QString storeImage(const QImage& image);

    NoteTextEdit* edit_;
    QToolBar* toolbar_;
    QComboBox* blockStyle_;
    QAction* boldAct_;
    QAction* italicAct_;
    QAction* underlineAct_;
    QAction* strikeAct_;
    QTimer saveTimer_;
    int64_t libraryId_ = 0;
    QString key_;
    QString lastSaved_;
    bool loading_ = false;
};

} // namespace fp::gui
