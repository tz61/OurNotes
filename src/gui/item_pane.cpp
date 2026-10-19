#include "gui/item_pane.h"

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/flow_layout.h"
#include "gui/icons.h"
#include "gui/note_editor.h"

namespace fp::gui {

namespace {

QLabel* sectionHeader(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setContentsMargins(0, 8, 0, 2);
    return l;
}

QLabel* fieldLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text + ":", parent);
    l->setAlignment(Qt::AlignRight | Qt::AlignTop);
    l->setContentsMargins(0, 4, 0, 0);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
    l->setPalette(p);
    return l;
}

QToolButton* smallButton(const QIcon& ic, const QString& tip, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(ic);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(14, 14));
    return b;
}

void clearLayout(QLayout* layout)
{
    while (auto* item = layout->takeAt(0)) {
        if (auto* w = item->widget())
            w->deleteLater();
        if (auto* l = item->layout()) {
            clearLayout(l);
            delete l;
        }
        delete item;
    }
}

} // namespace

// --- AutoTextEdit ------------------------------------------------------------------------

AutoTextEdit::AutoTextEdit(bool singleLine, QWidget* parent) : QPlainTextEdit(parent), singleLine_(singleLine)
{
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setTabChangesFocus(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    document()->setDocumentMargin(2);
    connect(document(), &QTextDocument::contentsChanged, this, &AutoTextEdit::updateHeight);
}

void AutoTextEdit::setTextSilently(const QString& text)
{
    QSignalBlocker b(this);
    setPlainText(text);
    document()->setModified(false);
    updateHeight();
}

QSize AutoTextEdit::sizeHint() const
{
    int lines = 0;
    for (auto b = document()->begin(); b.isValid(); b = b.next())
        lines += std::max(1, b.layout() ? b.layout()->lineCount() : 1);
    int h = fontMetrics().lineSpacing() * std::max(1, lines) + 2 * (int)document()->documentMargin() + 6;
    return {200, h};
}

void AutoTextEdit::updateHeight()
{
    int h = sizeHint().height();
    if (height() != h) {
        setFixedHeight(h);
        updateGeometry();
    }
}

void AutoTextEdit::resizeEvent(QResizeEvent* e)
{
    QPlainTextEdit::resizeEvent(e);
    QTimer::singleShot(0, this, &AutoTextEdit::updateHeight);
}

void AutoTextEdit::focusOutEvent(QFocusEvent* e)
{
    QPlainTextEdit::focusOutEvent(e);
    if (document()->isModified()) {
        document()->setModified(false);
        emit editingFinished();
    }
}

void AutoTextEdit::keyPressEvent(QKeyEvent* e)
{
    if (singleLine_ && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        document()->setModified(false);
        emit editingFinished();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

// --- InfoEditor ------------------------------------------------------------------------------

InfoEditor::InfoEditor(QWidget* parent) : QWidget(parent)
{
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(3);
    grid_->setColumnStretch(1, 1);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList& keys) {
                if (committing_ || key_.isEmpty() || lib != libraryId_)
                    return;
                if ((type == "item" && keys.contains(key_)) || type == "*")
                    QMetaObject::invokeMethod(this, &InfoEditor::rebuild, Qt::QueuedConnection);
            });
}

void InfoEditor::setItem(int64_t libraryId, const QString& key)
{
    libraryId_ = libraryId;
    key_ = key;
    rebuild();
}

void InfoEditor::rebuild()
{
    clearLayout(grid_);
    creators_.clear();
    auto& lib = AppContext::get().library();
    auto data = lib.item(libraryId_, ss(key_));
    if (!data)
        return;
    editable_ = AppContext::get().editable(libraryId_);
    auto type = jstr(*data, "itemType");
    int row = 0;

    // item type
    grid_->addWidget(fieldLabel(tr("Item Type"), this), row, 0);
    auto* typeBox = new QComboBox(this);
    for (auto& t : schema::itemTypes())
        typeBox->addItem(itemTypeIcon(qs(t)), qs(schema::typeLabel(t)), qs(t));
    typeBox->setCurrentIndex(typeBox->findData(qs(type)));
    typeBox->setEnabled(editable_);
    connect(typeBox, &QComboBox::activated, this, [this, typeBox](int i) { changeType(typeBox->itemData(i).toString()); });
    grid_->addWidget(typeBox, row++, 1);

    // title, then creators, then the remaining fields in schema order
    auto fields = schema::fieldsFor(type);
    auto addField = [&](const std::string& field) {
        grid_->addWidget(fieldLabel(qs(schema::fieldLabel(field)), this), row, 0);
        bool multi = field == "abstractNote" || field == "extra" || field == "title";
        QWidget* editor;
        auto value = jqs(*data, field.c_str());
        if (multi) {
            auto* e = new AutoTextEdit(field == "title", this);
            e->setTextSilently(value);
            e->setReadOnly(!editable_);
            connect(e, &AutoTextEdit::editingFinished, this,
                    [this, e, field] { commitField(field, e->toPlainText()); });
            editor = e;
        } else {
            auto* e = new QLineEdit(value, this);
            e->setReadOnly(!editable_);
            e->setFrame(false);
            connect(e, &QLineEdit::editingFinished, this, [this, e, field] {
                if (e->isModified()) {
                    e->setModified(false);
                    commitField(field, e->text());
                }
            });
            editor = e;
            if (field == "date")
                e->setPlaceholderText(tr("y-m-d"));
        }
        if ((field == "url" || field == "DOI") && !value.isEmpty()) {
            auto* box = new QWidget(this);
            auto* h = new QHBoxLayout(box);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(2);
            h->addWidget(editor, 1);
            auto* open = smallButton(icon("open"), tr("Open in browser"), box);
            QString url = field == "DOI" ? "https://doi.org/" + value : value;
            connect(open, &QToolButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
            h->addWidget(open);
            editor = box;
        }
        grid_->addWidget(editor, row++, 1);
    };
    if (std::find(fields.begin(), fields.end(), "title") != fields.end())
        addField("title");

    auto creators = data->value("creators", Json::array());
    Json blank{{"creatorType", schema::primaryCreatorType(type)}, {"firstName", ""}, {"lastName", ""}};
    int focusRow = -1;
    if (pendingBlank_ >= 0) {
        focusRow = std::min<int>(pendingBlank_, (int)creators.size());
        creators.insert(creators.begin() + focusRow, blank);
        pendingBlank_ = -1;
    }
    if (creators.empty())
        creators.push_back(blank);
    int i = 0;
    for (auto& c : creators) {
        bool single = c.contains("name");
        addCreatorRow(i++, type, jstr(c, "creatorType"), single, single ? jqs(c, "name") : jqs(c, "lastName"),
                      jqs(c, "firstName"), row++);
    }
    if (focusRow >= 0 && focusRow < (int)creators_.size())
        QTimer::singleShot(0, creators_[focusRow].last, qOverload<>(&QWidget::setFocus));
    for (auto& f : fields)
        if (f != "title")
            addField(f);

    auto addInfo = [&](const QString& label, const QString& value) {
        grid_->addWidget(fieldLabel(label, this), row, 0);
        auto* l = new QLabel(value, this);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setContentsMargins(2, 4, 0, 0);
        grid_->addWidget(l, row++, 1);
    };
    addInfo(tr("Date Added"), qs(isoToLocalDisplay(jstr(*data, "dateAdded"))));
    addInfo(tr("Modified"), qs(isoToLocalDisplay(jstr(*data, "dateModified"))));
    grid_->setRowStretch(row, 1);
}

void InfoEditor::addCreatorRow(int index, const std::string& itemType, const std::string& creatorType, bool single,
                               const QString& last, const QString& first, int gridRow)
{
    CreatorRow cr;
    cr.single = single;
    cr.type = new QComboBox(this);
    for (auto& ct : schema::creatorTypesFor(itemType))
        cr.type->addItem(qs(schema::creatorLabel(ct)) + ":", qs(ct));
    cr.type->setCurrentIndex(std::max(0, cr.type->findData(qs(creatorType))));
    cr.type->setEnabled(editable_);
    cr.type->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cr.type->setFrame(false);
    connect(cr.type, &QComboBox::activated, this, [this] { saveCreators(collectCreators(false)); });
    grid_->addWidget(cr.type, gridRow, 0, Qt::AlignRight | Qt::AlignTop);

    auto* box = new QWidget(this);
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    cr.last = new QLineEdit(last, box);
    cr.last->setMinimumWidth(30);
    cr.last->setPlaceholderText(single ? tr("name") : tr("last"));
    cr.last->setFrame(false);
    cr.last->setReadOnly(!editable_);
    h->addWidget(cr.last, 3);
    if (!single) {
        cr.first = new QLineEdit(first, box);
        cr.first->setMinimumWidth(30);
        cr.first->setPlaceholderText(tr("first"));
        cr.first->setFrame(false);
        cr.first->setReadOnly(!editable_);
        h->addWidget(cr.first, 2);
    }
    for (auto* e : {cr.last, cr.first})
        if (e)
            connect(e, &QLineEdit::editingFinished, this, [this, e] {
                if (e->isModified()) {
                    e->setModified(false);
                    saveCreators(collectCreators(false));
                }
            });
    if (editable_) {
        auto* swap = smallButton(icon("swap"), single ? tr("Switch to two fields") : tr("Switch to single field"), box);
        auto* remove = smallButton(icon("remove"), tr("Remove"), box);
        auto* add = smallButton(icon("add"), tr("Add creator"), box);
        h->addWidget(swap);
        h->addWidget(remove);
        h->addWidget(add);
        connect(swap, &QToolButton::clicked, this, [this, index] {
            auto arr = collectCreators(true);
            auto& c = arr[index];
            if (c.contains("name")) {
                auto parts = qs(jstr(c, "name")).split(' ', Qt::SkipEmptyParts);
                Json two{{"creatorType", c["creatorType"]},
                         {"firstName", ss(parts.mid(0, std::max<qsizetype>(0, parts.size() - 1)).join(' '))},
                         {"lastName", parts.isEmpty() ? std::string() : ss(parts.last())}};
                c = two;
            } else {
                Json one{{"creatorType", c["creatorType"]},
                         {"name", trim(jstr(c, "firstName") + " " + jstr(c, "lastName"))}};
                c = one;
            }
            saveCreators(arr);
        });
        connect(remove, &QToolButton::clicked, this, [this, index] {
            auto arr = collectCreators(true);
            arr.erase(arr.begin() + index);
            saveCreators(arr);
        });
        connect(add, &QToolButton::clicked, this, [this, index] {
            pendingBlank_ = index + 1;
            saveCreators(collectCreators(true));
        });
    }
    grid_->addWidget(box, gridRow, 1);
    creators_.push_back(cr);
}

void InfoEditor::commitField(const std::string& field, const QString& value)
{
    auto v = ss(field == "title" ? value.simplified() : value.trimmed());
    committing_ = true;
    AppContext::get().library().updateItem(libraryId_, ss(key_), [&](Json& d) {
        if (v.empty())
            d.erase(field);
        else
            d[field] = v;
    });
    committing_ = false;
}

Json InfoEditor::collectCreators(bool keepEmpty) const
{
    Json arr = Json::array();
    for (auto& c : creators_) {
        auto type = ss(c.type->currentData().toString());
        auto last = ss(c.last->text().trimmed());
        auto first = c.first ? ss(c.first->text().trimmed()) : std::string();
        if (!keepEmpty && last.empty() && first.empty())
            continue;
        if (c.single)
            arr.push_back(Json{{"creatorType", type}, {"name", last}});
        else
            arr.push_back(Json{{"creatorType", type}, {"firstName", first}, {"lastName", last}});
    }
    return arr;
}

void InfoEditor::saveCreators(Json creators)
{
    Json kept = Json::array();
    for (auto& c : creators)
        if (!trim(jstr(c, "name") + jstr(c, "firstName") + jstr(c, "lastName")).empty())
            kept.push_back(c);
    committing_ = true;
    AppContext::get().library().updateItem(libraryId_, ss(key_), [&](Json& d) { d["creators"] = kept; });
    committing_ = false;
    QMetaObject::invokeMethod(this, &InfoEditor::rebuild, Qt::QueuedConnection);
}

void InfoEditor::changeType(const QString& type)
{
    auto& lib = AppContext::get().library();
    auto data = lib.item(libraryId_, ss(key_));
    if (!data || jqs(*data, "itemType") == type)
        return;
    Json dropped;
    auto converted = schema::convertType(*data, ss(type), &dropped);
    if (!dropped.empty()) {
        QStringList names;
        for (auto& [k, v] : dropped.items())
            names << qs(schema::fieldLabel(k));
        if (QMessageBox::question(this, tr("Change Item Type"),
                                  tr("The following fields will be lost:\n\n%1\n\nContinue?").arg(names.join(", "))) !=
            QMessageBox::Yes) {
            QMetaObject::invokeMethod(this, &InfoEditor::rebuild, Qt::QueuedConnection);
            return;
        }
    }
    lib.saveItem(libraryId_, converted);
}

// --- TagsBox ----------------------------------------------------------------------------------

TagsBox::TagsBox(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    header_ = sectionHeader(tr("Tags"), this);
    v->addWidget(header_);
    auto* chips = new QWidget(this);
    flow_ = new FlowLayout(chips, 4);
    v->addWidget(chips);
    input_ = new QLineEdit(this);
    input_->setPlaceholderText(tr("Add tag…"));
    input_->setClearButtonEnabled(true);
    v->addWidget(input_);
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        auto tag = input_->text().trimmed();
        if (tag.isEmpty() || keys_.isEmpty())
            return;
        AppContext::get().library().addTag(libraryId_, toStdVector(keys_), ss(tag));
        input_->clear();
    });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList& keys) {
                if (lib != libraryId_)
                    return;
                for (auto& k : keys_)
                    if (keys.contains(k) || type == "*" || type == "setting") {
                        QMetaObject::invokeMethod(this, &TagsBox::rebuild, Qt::QueuedConnection);
                        return;
                    }
            });
}

void TagsBox::setItems(int64_t libraryId, const QStringList& keys)
{
    libraryId_ = libraryId;
    keys_ = keys;
    rebuild();
}

void TagsBox::rebuild()
{
    flow_->clear();
    auto& lib = AppContext::get().library();
    bool editable = AppContext::get().editable(libraryId_);
    input_->setVisible(editable);
    std::map<std::string, int> counts;
    for (auto& d : lib.items(libraryId_, toStdVector(keys_)))
        for (auto& t : d.value("tags", Json::array()))
            counts[jstr(t, "tag")]++;
    auto colors = lib.tagColors(libraryId_);
    header_->setText(tr("Tags (%1)").arg(counts.size()));
    for (auto& [tag, n] : counts) {
        auto* chip = new QToolButton;
        chip->setText(qs(tag) + (keys_.size() > 1 && n < keys_.size() ? QString(" (%1)").arg(n) : QString()));
        chip->setAutoRaise(false);
        if (colors.count(tag))
            chip->setIcon(colorIcon(qs(colors[tag]), 10));
        chip->setToolTip(editable ? tr("Click to remove") : QString());
        chip->setEnabled(editable);
        if (editable) {
            chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            connect(chip, &QToolButton::clicked, this, [this, tag = tag] {
                AppContext::get().library().removeTag(libraryId_, toStdVector(keys_), tag);
            });
        }
        flow_->addWidget(chip);
    }
    // completion with all tags of the library
    QStringList all;
    for (auto& t : lib.tags(libraryId_))
        all << qs(t.name);
    auto* completer = new QCompleter(all, input_);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    input_->setCompleter(completer);
}

// --- RelatedBox ---------------------------------------------------------------------------------

RelatedBox::RelatedBox(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* head = new QHBoxLayout;
    header_ = sectionHeader(tr("Related"), this);
    head->addWidget(header_, 1);
    auto* add = smallButton(icon("add"), tr("Add related item"), this);
    head->addWidget(add);
    v->addLayout(head);
    list_ = new QVBoxLayout;
    list_->setSpacing(0);
    v->addLayout(list_);
    connect(add, &QToolButton::clicked, this, [this] {
        // simple picker: search the library by title/creator
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Add Related Item"));
        auto* layout = new QVBoxLayout(&dlg);
        auto* search = new QLineEdit(&dlg);
        search->setPlaceholderText(tr("Search title, creator, year"));
        auto* results = new QListWidget(&dlg);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(search);
        layout->addWidget(results);
        layout->addWidget(buttons);
        dlg.resize(560, 420);
        auto& lib = AppContext::get().library();
        auto rows = lib.itemRows(libraryId_);
        auto fill = [&] {
            results->clear();
            auto keys = lib.search(libraryId_, ss(search->text()), SearchMode::TitleCreatorYear);
            for (auto& r : rows) {
                if (!r.parentKey.empty() || r.trashed || qs(r.key) == key_)
                    continue;
                if (!search->text().trimmed().isEmpty() && !keys.count(r.key))
                    continue;
                auto* it = new QListWidgetItem(itemTypeIcon(qs(r.itemType)),
                                               qs(r.title) + (r.creator.empty() ? "" : "  —  " + qs(r.creator)), results);
                it->setData(Qt::UserRole, qs(r.key));
            }
        };
        fill();
        connect(search, &QLineEdit::textChanged, &dlg, fill);
        connect(results, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() == QDialog::Accepted && results->currentItem())
            lib.addRelated(libraryId_, ss(key_), ss(results->currentItem()->data(Qt::UserRole).toString()));
    });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList& keys) {
                if (lib == libraryId_ && (keys.contains(key_) || type == "*"))
                    QMetaObject::invokeMethod(this, &RelatedBox::rebuild, Qt::QueuedConnection);
            });
}

void RelatedBox::setItem(int64_t libraryId, const QString& key)
{
    libraryId_ = libraryId;
    key_ = key;
    rebuild();
}

void RelatedBox::rebuild()
{
    clearLayout(list_);
    auto& lib = AppContext::get().library();
    auto related = lib.relatedKeys(libraryId_, ss(key_));
    header_->setText(tr("Related (%1)").arg(related.size()));
    for (auto& k : related) {
        auto d = lib.item(libraryId_, k);
        if (!d)
            continue;
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* link = new QPushButton(itemTypeIcon(jqs(*d, "itemType")),
                                     jstr(*d, "itemType") == "note" ? qs(noteTitle(jstr(*d, "note"))) : jqs(*d, "title"),
                                     row);
        link->setFlat(true);
        link->setStyleSheet("text-align: left");
        connect(link, &QPushButton::clicked, this, [this, k = qs(k)] { emit itemActivated(k); });
        auto* remove = smallButton(icon("remove"), tr("Remove relation"), row);
        connect(remove, &QToolButton::clicked, this,
                [this, k] { AppContext::get().library().removeRelated(libraryId_, ss(key_), k); });
        h->addWidget(link, 1);
        h->addWidget(remove);
        list_->addWidget(row);
    }
}

// --- AttachmentBox ------------------------------------------------------------------------------

AttachmentBox::AttachmentBox(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    title_ = new AutoTextEdit(true, this);
    QFont f = title_->font();
    f.setPointSizeF(f.pointSizeF() * 1.2);
    f.setBold(true);
    title_->setFont(f);
    v->addWidget(title_);
    info_ = new QLabel(this);
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(info_);
    file_ = new QLabel(this);
    file_->setWordWrap(true);
    file_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(file_);
    buttons_ = new QWidget(this);
    auto* h = new QHBoxLayout(buttons_);
    h->setContentsMargins(0, 4, 0, 4);
    auto* open = new QPushButton(icon("open"), tr("Open"), buttons_);
    auto* show = new QPushButton(tr("Show File"), buttons_);
    auto* rename = new QPushButton(tr("Rename from Parent"), buttons_);
    rename->setToolTip(tr("Rename the file to \"Creator - Year - Title\" using the parent item's metadata"));
    h->addWidget(open);
    h->addWidget(show);
    h->addWidget(rename);
    h->addStretch();
    v->addWidget(buttons_);
    v->addWidget(sectionHeader(tr("Attachment Note"), this));
    note_ = new AutoTextEdit(false, this);
    note_->setPlaceholderText(tr("Add a note about this attachment"));
    v->addWidget(note_);

    connect(open, &QPushButton::clicked, this, [this] { emit openRequested(key_); });
    connect(show, &QPushButton::clicked, this, [this] {
        auto& lib = AppContext::get().library();
        if (auto d = lib.item(libraryId_, ss(key_))) {
            auto p = lib.attachmentFile(libraryId_, *d);
            if (!p.empty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(p.parent_path())));
        }
    });
    connect(rename, &QPushButton::clicked, this, &AttachmentBox::renameFromParent);
    connect(title_, &AutoTextEdit::editingFinished, this, [this] {
        AppContext::get().library().updateItem(libraryId_, ss(key_),
                                               [&](Json& d) { d["title"] = ss(title_->toPlainText().simplified()); });
    });
    connect(note_, &AutoTextEdit::editingFinished, this, [this] {
        auto text = note_->toPlainText().trimmed();
        AppContext::get().library().updateItem(libraryId_, ss(key_), [&](Json& d) {
            if (text.isEmpty())
                d.erase("note");
            else
                d["note"] = textToNoteHtml(ss(text));
        });
    });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList& keys) {
                if (lib == libraryId_ && (keys.contains(key_) || type == "*") && !isAncestorOf(QApplication::focusWidget()))
                    QMetaObject::invokeMethod(this, &AttachmentBox::rebuild, Qt::QueuedConnection);
            });
}

void AttachmentBox::setItem(int64_t libraryId, const QString& key)
{
    libraryId_ = libraryId;
    key_ = key;
    rebuild();
}

void AttachmentBox::rebuild()
{
    auto& lib = AppContext::get().library();
    auto d = lib.item(libraryId_, ss(key_));
    if (!d)
        return;
    bool editable = AppContext::get().editable(libraryId_);
    title_->setTextSilently(jqs(*d, "title"));
    title_->setReadOnly(!editable);
    note_->setTextSilently(qs(htmlToText(jstr(*d, "note"))));
    note_->setReadOnly(!editable);
    auto mode = jstr(*d, "linkMode");
    QStringList lines;
    if (mode == "linked_url") {
        lines << tr("Link: <a href=\"%1\">%1</a>").arg(jqs(*d, "url").toHtmlEscaped());
        file_->clear();
    } else {
        auto path = lib.attachmentFile(libraryId_, *d);
        QFileInfo fi(qpath(path));
        lines << tr("Filename: %1").arg(jqs(*d, "filename").toHtmlEscaped());
        if (fi.exists())
            lines << tr("Size: %1").arg(qs(humanSize((uint64_t)fi.size())));
        else
            lines << "<span style=\"color:#d63c3c\">" + tr("The file is missing.") + "</span>";
        if (!jstr(*d, "url").empty())
            lines << tr("Original URL: <a href=\"%1\">%1</a>").arg(jqs(*d, "url").toHtmlEscaped());
        int anns = lib.annotationCount(libraryId_, ss(key_));
        if (anns)
            lines << tr("Annotations: %1").arg(anns);
        file_->setText("<small>" + qpath(path).toHtmlEscaped() + "</small>");
    }
    lines << tr("Added: %1").arg(qs(isoToLocalDisplay(jstr(*d, "dateAdded"))));
    info_->setText(lines.join("<br>"));
    info_->setOpenExternalLinks(true);
    buttons_->setVisible(true);
}

void AttachmentBox::renameFromParent()
{
    auto& lib = AppContext::get().library();
    auto d = lib.item(libraryId_, ss(key_));
    if (!d)
        return;
    auto parent = lib.item(libraryId_, jstr(*d, "parentItem"));
    auto path = lib.attachmentFile(libraryId_, *d);
    if (!parent || path.empty() || !fs::exists(path)) {
        QMessageBox::information(this, tr("Rename File"), tr("This attachment has no parent item or no file."));
        return;
    }
    std::string base;
    auto creator = creatorSummary(parent->value("creators", Json::array()),
                                  schema::primaryCreatorType(jstr(*parent, "itemType")));
    auto year = yearOf(jstr(*parent, "date"));
    if (!creator.empty())
        base += creator + " - ";
    if (!year.empty())
        base += year + " - ";
    auto title = jstr(*parent, "title");
    if (title.size() > 100)
        title = title.substr(0, 100);
    base += title;
    auto newName = safeFilename(base + pathString(path.extension()));
    auto target = path.parent_path() / u8path(newName);
    if (target == path)
        return;
    std::error_code ec;
    fs::rename(path, target, ec);
    if (ec) {
        QMessageBox::warning(this, tr("Rename File"), qs(ec.message()));
        return;
    }
    lib.updateItem(libraryId_, ss(key_), [&](Json& data) {
        data["filename"] = newName;
        if (jstr(data, "linkMode") == "linked_file")
            data["path"] = pathString(target);
    });
}

// --- ItemPane ----------------------------------------------------------------------------------

static QScrollArea* scrollPage(QWidget* content, QWidget* parent)
{
    auto* area = new QScrollArea(parent);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    return area;
}

ItemPane::ItemPane(QWidget* parent) : QStackedWidget(parent)
{
    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    QPalette p = message_->palette();
    p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
    message_->setPalette(p);
    addWidget(message_);

    // regular item
    auto* itemContent = new QWidget;
    auto* iv = new QVBoxLayout(itemContent);
    iv->setContentsMargins(10, 8, 10, 8);
    iv->addWidget(sectionHeader(tr("Info"), itemContent));
    info_ = new InfoEditor(itemContent);
    iv->addWidget(info_);
    tags_ = new TagsBox(itemContent);
    iv->addWidget(tags_);
    related_ = new RelatedBox(itemContent);
    iv->addWidget(related_);
    iv->addStretch();
    itemPage_ = scrollPage(itemContent, this);
    addWidget(itemPage_);

    // note
    notePage_ = new QWidget(this);
    auto* nv = new QVBoxLayout(notePage_);
    nv->setContentsMargins(0, 0, 0, 0);
    note_ = new NoteEditor(notePage_);
    nv->addWidget(note_, 1);
    auto* noteExtras = new QWidget(notePage_);
    auto* nev = new QVBoxLayout(noteExtras);
    nev->setContentsMargins(10, 0, 10, 6);
    noteTags_ = new TagsBox(noteExtras);
    noteRelated_ = new RelatedBox(noteExtras);
    nev->addWidget(noteTags_);
    nev->addWidget(noteRelated_);
    nv->addWidget(noteExtras);
    addWidget(notePage_);

    // attachment
    auto* attContent = new QWidget;
    auto* av = new QVBoxLayout(attContent);
    av->setContentsMargins(10, 8, 10, 8);
    attachment_ = new AttachmentBox(attContent);
    av->addWidget(attachment_);
    attachmentTags_ = new TagsBox(attContent);
    av->addWidget(attachmentTags_);
    av->addStretch();
    attachmentPage_ = scrollPage(attContent, this);
    addWidget(attachmentPage_);

    connect(attachment_, &AttachmentBox::openRequested, this, &ItemPane::openAttachment);
    connect(related_, &RelatedBox::itemActivated, this, &ItemPane::selectItem);
    connect(noteRelated_, &RelatedBox::itemActivated, this, &ItemPane::selectItem);
    showItems(0, {});
}

void ItemPane::showItems(int64_t libraryId, const QStringList& keys)
{
    if (keys.size() != 1) {
        note_->clear();
        message_->setText(keys.isEmpty() ? tr("No item selected") : tr("%n items selected", nullptr, keys.size()));
        setCurrentWidget(message_);
        return;
    }
    auto d = AppContext::get().library().item(libraryId, ss(keys[0]));
    if (!d) {
        setCurrentWidget(message_);
        return;
    }
    auto type = jstr(*d, "itemType");
    if (type == "note") {
        note_->setNote(libraryId, keys[0]);
        noteTags_->setItems(libraryId, keys);
        noteRelated_->setItem(libraryId, keys[0]);
        setCurrentWidget(notePage_);
    } else if (type == "attachment") {
        note_->clear();
        attachment_->setItem(libraryId, keys[0]);
        attachmentTags_->setItems(libraryId, keys);
        setCurrentWidget(attachmentPage_);
    } else {
        note_->clear();
        info_->setItem(libraryId, keys[0]);
        tags_->setItems(libraryId, keys);
        related_->setItem(libraryId, keys[0]);
        setCurrentWidget(itemPage_);
    }
}

} // namespace fp::gui
