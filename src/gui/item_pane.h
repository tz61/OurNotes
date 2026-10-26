#pragma once

#include <QPlainTextEdit>
#include <QStackedWidget>

#include <vector>

#include "core/json.h"
#include "gui/items_model.h"

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace on::gui {

class FlowLayout;
class NoteEditor;

// Plain-text field that grows with its content; emits editingFinished on focus
// loss (and on Enter when single-line).
class AutoTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit AutoTextEdit(bool singleLine, QWidget* parent = nullptr);
    void setTextSilently(const QString& text);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {40, sizeHint().height()}; }

signals:
    void editingFinished();

protected:
    void focusOutEvent(QFocusEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateHeight();
    bool singleLine_;
};

// Metadata form of a regular item: type, creators and all fields of its type.
class InfoEditor : public QWidget {
    Q_OBJECT
public:
    explicit InfoEditor(QWidget* parent = nullptr);
    void setItem(int64_t libraryId, const QString& key);
    void rebuild();
    void refresh();

protected:
    void showEvent(QShowEvent* e) override;  // (from the Notes tab) laid out before drawn, too

private:
    void settleLayout();
    struct CreatorRow {
        QComboBox* type = nullptr;
        QLineEdit* last = nullptr;
        QLineEdit* first = nullptr;
        bool single = false;
    };
    void commitField(const std::string& field, const QString& value);
    Json collectCreators(bool keepEmpty) const;
    void saveCreators(Json creators);
    void changeType(const QString& type);
    void addCreatorRow(int index, const std::string& itemType, const std::string& creatorType, bool single,
                       const QString& last, const QString& first, int gridRow);

    QGridLayout* grid_;
    int64_t libraryId_ = 0;
    QString key_;
    bool editable_ = true;
    bool committing_ = false;
    bool stale_ = false;  // changed while a field was being typed in: shown once it is left
    int pendingBlank_ = -1;  // index of a new, still empty creator row (UI only)
    std::vector<CreatorRow> creators_;
};

class TagsBox : public QWidget {
    Q_OBJECT
public:
    explicit TagsBox(QWidget* parent = nullptr);
    void setItems(int64_t libraryId, const QStringList& keys);
    void rebuild();

private:
    void confirmRemove(const QString& tag);

    QLabel* header_;
    FlowLayout* flow_;
    QLineEdit* input_;
    int64_t libraryId_ = 0;
    QStringList keys_;
};

class RelatedBox : public QWidget {
    Q_OBJECT
public:
    explicit RelatedBox(QWidget* parent = nullptr);
    void setItem(int64_t libraryId, const QString& key);
    void rebuild();

signals:
    void itemActivated(const QString& key);

private:
    QLabel* header_;
    QVBoxLayout* list_;
    int64_t libraryId_ = 0;
    QString key_;
};

class AttachmentBox : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentBox(QWidget* parent = nullptr);
    void setItem(int64_t libraryId, const QString& key);
    void rebuild();

signals:
    void openRequested(const QString& key);

private:
    void renameFromParent();
    AutoTextEdit* title_;
    QLabel* info_;
    QLabel* file_;
    QWidget* buttons_;
    AutoTextEdit* note_;
    int64_t libraryId_ = 0;
    QString key_;
};

// The notes of one item, shown next to its Info: a list (when there are several)
// and an editor for the selected note.
class ItemNotesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ItemNotesPanel(QWidget* parent = nullptr);
    void setItem(int64_t libraryId, const QString& key);
    int count() const { return count_; }
    NoteEditor* editor() const { return editor_; }
    // The same with no note, one or several (the list and the editor come and go):
    // otherwise the window's minimum size changed from item to item, and at a
    // fractional screen scale the window lost a pixel of width each time.
    QSize minimumSizeHint() const override;

signals:
    void countChanged(int count);

private:
    void reload();
    void addNote();

    int64_t libraryId_ = 0;
    QString key_;
    int count_ = 0;
    QLabel* header_;
    QToolButton* add_;
    QListWidget* list_;
    QLabel* empty_;
    NoteEditor* editor_;
};

// Where an item is filed: its library and the path of every collection it is in
// ("My Library › Robotics › Humanoid"), or Unfiled / Trash. Each part is a link.
// Notes and attachments show where their parent item is.
class LocationBox : public QWidget {
    Q_OBJECT
public:
    explicit LocationBox(QWidget* parent = nullptr);
    void setItem(int64_t libraryId, const QString& key);
    QStringList paths() const;  // the lines as plain text, e.g. for tests
    // A line, whatever the item: one line per collection it is in made the window's
    // minimum height change from item to item, and a short window grew and shrank as
    // the selection moved. Short of room, the lines below the first are cut off.
    QSize minimumSizeHint() const override;

signals:
    void scopeRequested(const Scope& scope);

protected:
    void changeEvent(QEvent* e) override;  // the icons and separators in the theme's colours

private:
    void rebuild();

    int64_t libraryId_ = 0;
    QString key_;
    QVBoxLayout* lines_;
    QStringList paths_;
};

// Right pane of the library tab.
class ItemPane : public QStackedWidget {
    Q_OBJECT
public:
    explicit ItemPane(QWidget* parent = nullptr);
    void showItems(int64_t libraryId, const QStringList& keys);
    NoteEditor* noteEditor() const { return note_; }
    // Info (0) or Notes (1) beside a regular item, as a click on the tab does (for every
    // item and PDF)
    void showInfoOrNotes(int tab);

signals:
    void openAttachment(const QString& key);
    void selectItem(const QString& key);
    // A collection (or Unfiled, Trash, the library) was chosen in the item's location.
    void showScope(const Scope& scope, const QString& itemKey);

private:
    QLabel* message_;
    QList<LocationBox*> locations_;  // one per page
    QString currentKey_;
    QWidget* itemContainer_;  // location + itemPage_
    QTabWidget* itemPage_;
    ItemNotesPanel* itemNotes_;
    InfoEditor* info_;
    TagsBox* tags_;
    RelatedBox* related_;
    QWidget* notePage_;
    NoteEditor* note_;
    TagsBox* noteTags_;
    RelatedBox* noteRelated_;
    QWidget* attachmentPage_;
    AttachmentBox* attachment_;
    TagsBox* attachmentTags_;
};

} // namespace on::gui
