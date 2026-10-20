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
#include <QMenu>
#include <QSplitter>
#include <QTabWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/flow_layout.h"
#include "gui/icons.h"
#include "gui/note_editor.h"
#include "gui/side_pane.h"

namespace on::gui {

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
    l->setForegroundRole(QPalette::PlaceholderText);  // dimmed, follows theme switches
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
        if (auto* w = item->widget()) {
            w->hide();  // at once: deleted later, the old rows were drawn over the new for a frame
            w->deleteLater();
        }
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
    updateHeight();  // now: a frame later, the rows below it were drawn where they were not to stay
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
            e->setCursorPosition(0);  // a long value shows its start, not its end
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
    if (isVisible())
        settleLayout();
}

// In place before anything is drawn. Shown by the layout later, the new rows took a frame
// at a default width, where the title and the abstract are many lines tall, and the
// creators below then jumped up. Laid out once for the widths, once more for the heights
// the text boxes take at them.
void InfoEditor::settleLayout()
{
    for (int i = 0; i < grid_->count(); ++i)
        if (auto* w = grid_->itemAt(i)->widget())
            w->show();
    grid_->activate();
    grid_->activate();
}

void InfoEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    settleLayout();
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
    cr.last->setCursorPosition(0);
    cr.last->setReadOnly(!editable_);
    h->addWidget(cr.last, 3);
    if (!single) {
        cr.first = new QLineEdit(first, box);
        cr.first->setMinimumWidth(30);
        cr.first->setPlaceholderText(tr("first"));
        cr.first->setFrame(false);
        cr.first->setCursorPosition(0);
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

// --- TagChip -----------------------------------------------------------------------------------

// Round × button inside a tag chip; highlighted when hovered.
class TagRemoveButton : public QAbstractButton {
public:
    explicit TagRemoveButton(QWidget* parent) : QAbstractButton(parent)
    {
        setObjectName("removeTag");
        setToolTip(tr("Remove tag…"));
        setAccessibleName(tr("Remove tag"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        bool hot = underMouse() || hasFocus() || isDown();
        QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QColor fill = hot ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid);
        if (!hot)
            fill.setAlphaF(0.6);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(r);
        qreal d = r.width() * 0.22;
        QPointF c = r.center();
        p.setPen(QPen(hot ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::ButtonText),
                      std::max(1.4, r.width() / 9.0), Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-d, -d), c + QPointF(d, d));
        p.drawLine(c + QPointF(d, -d), c + QPointF(-d, d));
    }
};

// A tag shown as a rounded pill: optional colour dot, elided text, and a remove
// button when the library is editable. Clicking the tag itself changes nothing.
class TagChip : public QWidget {
public:
    TagChip(const QString& tag, const QString& color, bool automatic, bool removable, QWidget* parent = nullptr)
        : QWidget(parent), tag_(tag), color_(color), automatic_(automatic)
    {
        setObjectName("tagChip");
        setToolTip(automatic ? tr("%1 (automatic tag)").arg(tag) : tag);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover);
        if (removable) {
            remove_ = new TagRemoveButton(this);
            connect(remove_, &QAbstractButton::clicked, this, [this] {
                if (onRemove)
                    onRemove();
            });
        }
    }

    std::function<void()> onRemove;

    QSize sizeHint() const override
    {
        QFontMetrics fm(chipFont());
        // +4: italic glyphs overhang their advance width
        int w = padding() * 2 + fm.horizontalAdvance(tag_) + 4 + (color_.isEmpty() ? 0 : dot() + 6) +
                (remove_ ? buttonSize() + 2 : 0);
        return {w, std::max(fm.height() + 8, buttonSize() + 4)};
    }
    QSize minimumSizeHint() const override { return {padding() * 2 + 24 + (remove_ ? buttonSize() : 0), sizeHint().height()}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(palette().color(QPalette::Button));
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        qreal x = padding();
        if (!color_.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(color_));
            p.drawEllipse(QRectF(x, (height() - dot()) / 2.0, dot(), dot()));
            x += dot() + 6;
        }
        qreal right = remove_ ? remove_->geometry().left() - 2 : width() - padding();
        p.setFont(chipFont());
        QColor fg = palette().color(QPalette::ButtonText);
        if (automatic_)
            fg.setAlphaF(0.8);
        p.setPen(fg);
        QString text = QFontMetrics(chipFont()).elidedText(tag_, Qt::ElideRight, int(right - x));
        p.drawText(QRectF(x, 0, right - x, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        if (remove_) {
            int s = buttonSize();
            remove_->setGeometry(width() - s - (height() - s) / 2, (height() - s) / 2, s, s);
        }
    }
    void changeEvent(QEvent* e) override
    {
        if (e->type() == QEvent::FontChange)
            updateGeometry();
        QWidget::changeEvent(e);
    }

private:
    QFont chipFont() const
    {
        QFont f = font();
        f.setItalic(automatic_);
        return f;
    }
    int padding() const { return 9; }
    int dot() const { return 8; }
    int buttonSize() const { return QFontMetrics(font()).height(); }

    QString tag_, color_;
    bool automatic_;
    TagRemoveButton* remove_ = nullptr;
};

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
                // of the settings, only tag colours matter (not e.g. reading positions)
                bool colors = type == "setting" && keys.contains("tagColors");
                for (auto& k : keys_)
                    if (keys.contains(k) || type == "*" || colors) {
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
    std::map<std::string, int> types;
    for (auto& d : lib.items(libraryId_, toStdVector(keys_)))
        for (auto& t : d.value("tags", Json::array()))
            types[jstr(t, "tag")] = (int)jint(t, "type");
    for (auto& [tag, n] : counts) {
        QString label = qs(tag) + (keys_.size() > 1 && n < keys_.size() ? QString(" (%1)").arg(n) : QString());
        auto* chip = new TagChip(label, colors.count(tag) ? qs(colors[tag]) : QString(), types[tag] == 1, editable);
        // asked from the box, not the chip: a rebuild while the dialog is open deletes the chip
        chip->onRemove = [this, tag = tag] {
            QMetaObject::invokeMethod(this, [this, tag] { confirmRemove(qs(tag)); }, Qt::QueuedConnection);
        };
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

void TagsBox::confirmRemove(const QString& tag)
{
    if (keys_.isEmpty())
        return;
    QMessageBox box(QMessageBox::Question, tr("Remove Tag"),
                    keys_.size() == 1 ? tr("Remove the tag \"%1\" from this item?").arg(tag)
                                      : tr("Remove the tag \"%1\" from the %2 selected items?")
                                            .arg(tag)
                                            .arg(keys_.size()),
                    QMessageBox::NoButton, this);
    box.setObjectName("confirmRemoveTag");
    auto* remove = box.addButton(tr("Remove"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(remove);
    box.exec();
    if (box.clickedButton() == remove)
        AppContext::get().library().removeTag(libraryId_, toStdVector(keys_), ss(tag));
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

// --- ItemNotesPanel ------------------------------------------------------------------------------

ItemNotesPanel::ItemNotesPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 6, 0, 0);
    v->setSpacing(4);
    auto* head = new QHBoxLayout;
    head->setContentsMargins(10, 0, 6, 0);
    header_ = new QLabel(this);
    add_ = new QToolButton(this);
    add_->setIcon(icon("new-note"));
    add_->setText(tr("Add Note"));
    add_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    add_->setAutoRaise(true);
    // room for "99 notes" whatever the count, like the list and the editor below (see
    // minimumSizeHint)
    header_->setMinimumWidth(header_->fontMetrics().horizontalAdvance(tr("%1 notes").arg(99)));
    head->addWidget(header_, 1);
    head->addWidget(add_);
    v->addLayout(head);
    auto* split = new QSplitter(Qt::Vertical, this);
    list_ = new QListWidget(split);
    editor_ = new NoteEditor(split);
    shareNotesSplit(split);
    v->addWidget(split, 1);
    empty_ = new QLabel(tr("No notes yet."), this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(empty_, 1);

    connect(add_, &QToolButton::clicked, this, &ItemNotesPanel::addNote);
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item)
            editor_->setNote(libraryId_, item->data(Qt::UserRole).toString());
    });
    enableNoteTrash(list_, [this] { return libraryId_; });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList&) {
                if (!key_.isEmpty() && (lib == libraryId_ || lib == 0) && (type == "item" || type == "*"))
                    QMetaObject::invokeMethod(this, &ItemNotesPanel::reload, Qt::QueuedConnection);
            });
}

QSize ItemNotesPanel::minimumSizeHint() const
{
    int head = std::max(header_->sizeHint().height(), add_->sizeHint().height());
    int width = std::max({QWidget::minimumSizeHint().width(), list_->minimumSizeHint().width(),
                          editor_->minimumSizeHint().width()});
    return {width, 6 + head + 4 + editor_->minimumSizeHint().height()};
}

void ItemNotesPanel::setItem(int64_t libraryId, const QString& key)
{
    if (libraryId == libraryId_ && key == key_)
        return;
    editor_->flush();
    libraryId_ = libraryId;
    key_ = key;
    list_->clear();
    reload();
}

void ItemNotesPanel::reload()
{
    auto& lib = AppContext::get().library();
    std::vector<Json> notes;
    for (auto& n : lib.children(libraryId_, ss(key_), "note"))
        if (!jbool(n, "deleted"))
            notes.push_back(n);
    // most recently edited first
    std::stable_sort(notes.begin(), notes.end(), [](const Json& a, const Json& b) {
        return jstr(a, "dateModified") > jstr(b, "dateModified");
    });
    QString current = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : editor_->key();
    {
        QSignalBlocker block(list_);
        list_->clear();
        for (auto& n : notes) {
            auto title = qs(noteTitle(jstr(n, "note")));
            auto* item = new QListWidgetItem(icon("type-note"), title.isEmpty() ? tr("(empty note)") : title, list_);
            item->setData(Qt::UserRole, jqs(n, "key"));
            item->setToolTip(tr("Modified %1").arg(qs(isoToLocalDisplay(jstr(n, "dateModified")))));
            if (jqs(n, "key") == current)
                list_->setCurrentItem(item);
        }
        if (!list_->currentItem() && list_->count())
            list_->setCurrentRow(0);
    }
    int n = (int)notes.size();
    bool changed = n != count_;
    count_ = n;
    header_->setText(n == 0 ? QString() : n == 1 ? tr("1 note") : tr("%1 notes").arg(n));
    list_->setVisible(n > 1);
    editor_->setVisible(n > 0);
    empty_->setVisible(n == 0);
    add_->setEnabled(AppContext::get().editable(libraryId_));
    if (list_->currentItem())
        editor_->setNote(libraryId_, list_->currentItem()->data(Qt::UserRole).toString());
    else
        editor_->clear();
    if (changed)
        emit countChanged(n);
}

void ItemNotesPanel::addNote()
{
    auto note = AppContext::get().library().createNote(libraryId_, "", ss(key_));
    reload();
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == jqs(note, "key"))
            list_->setCurrentRow(i);
    editor_->setNote(libraryId_, jqs(note, "key"));
    editor_->focusEditor();
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

// --- LocationBox --------------------------------------------------------------------------------

LocationBox::LocationBox(QWidget* parent) : QWidget(parent)
{
    setObjectName("itemLocation");
    lines_ = new QVBoxLayout(this);
    lines_->setContentsMargins(0, 0, 0, 4);
    lines_->setSpacing(2);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList&) {
                if (!key_.isEmpty() && (lib == libraryId_ || lib == 0) &&
                    (type == "item" || type == "collection" || type == "library" || type == "*"))
                    QMetaObject::invokeMethod(this, &LocationBox::rebuild, Qt::QueuedConnection);
            });
}

void LocationBox::setItem(int64_t libraryId, const QString& key)
{
    libraryId_ = libraryId;
    key_ = key;
    rebuild();
}

QStringList LocationBox::paths() const { return paths_; }

QSize LocationBox::minimumSizeHint() const
{
    return {QWidget::minimumSizeHint().width(), fontMetrics().height() + 6};
}

void LocationBox::rebuild()
{
    // later: rebuilding can be caused by a click on one of these links
    while (auto* item = lines_->takeAt(0)) {
        if (auto* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    paths_.clear();
    auto& lib = AppContext::get().library();
    auto item = lib.item(libraryId_, ss(key_));
    auto info = lib.library(libraryId_);
    if (!item || !info)
        return;
    // notes and attachments are filed with their parent item
    auto top = *item;
    if (auto parent = jstr(*item, "parentItem"); !parent.empty())
        if (auto p = lib.item(libraryId_, parent))
            top = *p;
    QString sep = QString(" <span style=\"color:%1\">\u203a</span> ")
                      .arg(palette().color(QPalette::PlaceholderText).name());
    auto link = [](const QString& href, const QString& text) {
        return QString("<a href=\"%1\">%2</a>").arg(href, text.toHtmlEscaped());
    };
    QString root = link("library", qs(info->name));
    struct Line {
        QString plain, html;
    };
    std::vector<Line> lines;
    if (jbool(*item, "deleted") || jbool(top, "deleted")) {
        lines.push_back({qs(info->name) + " \u203a " + tr("Trash"), root + sep + link("trash", tr("Trash"))});
    } else {
        for (auto& c : top.value("collections", Json::array())) {
            if (!c.is_string())
                continue;
            // walk up to the top-level collection
            QStringList names, hrefs;
            std::string key = c.get<std::string>();
            for (int depth = 0; !key.empty() && depth < 32; ++depth) {
                auto coll = lib.collection(libraryId_, key);
                if (!coll)
                    break;
                names.prepend(jqs(*coll, "name"));
                hrefs.prepend("collection:" + qs(key));
                auto parent = coll->value("parentCollection", Json());
                key = parent.is_string() ? parent.get<std::string>() : std::string();
            }
            if (names.isEmpty())
                continue;
            QString html = root, plain = qs(info->name);
            for (int i = 0; i < names.size(); ++i) {
                html += sep + link(hrefs[i], names[i]);
                plain += " \u203a " + names[i];
            }
            lines.push_back({plain, html});
        }
        if (lines.empty())
            lines.push_back({qs(info->name) + " \u203a " + tr("Unfiled Items"), root + sep + link("unfiled", tr("Unfiled Items"))});
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.plain.localeAwareCompare(b.plain) < 0; });
    bool trashed = jbool(*item, "deleted") || jbool(top, "deleted");
    for (auto& l : lines) {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        auto* mark = new QLabel(row);
        int s = fontMetrics().height();
        mark->setPixmap(icon(trashed ? "trash" : "folder").pixmap(QSize(s, s), devicePixelRatioF()));
        mark->setAlignment(Qt::AlignTop);
        h->addWidget(mark);
        auto* label = new QLabel(row);
        h->addWidget(label, 1);
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setText(l.html);
        label->setToolTip(l.plain);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
        connect(label, &QLabel::linkActivated, this, [this](const QString& href) {
            Scope s{libraryId_, ScopeKind::Library, {}};
            if (href == "trash")
                s.kind = ScopeKind::Trash;
            else if (href == "unfiled")
                s.kind = ScopeKind::Unfiled;
            else if (href.startsWith("collection:")) {
                s.kind = ScopeKind::Collection;
                s.collectionKey = ss(href.mid(11));
            }
            emit scopeRequested(s);
        });
        lines_->addWidget(row);
        paths_ << l.plain;
    }
    if (trashed && AppContext::get().editable(libraryId_)) {
        // what is in the trash: the item, or the item it belongs to
        auto trashedKey = jbool(*item, "deleted") ? key_ : jqs(top, "key");
        auto title = qs(jstr(jbool(*item, "deleted") ? *item : top, "title"));
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 2, 0, 0);
        auto* restore = new QPushButton(tr("Restore to Library"), row);
        restore->setObjectName("restoreFromTrash");
        auto* erase = new QPushButton(tr("Delete Permanently…"), row);
        erase->setObjectName("deletePermanently");
        h->addWidget(restore);
        h->addWidget(erase);
        h->addStretch();
        connect(restore, &QPushButton::clicked, this, [this, trashedKey] {
            AppContext::get().library().trashItems(libraryId_, {ss(trashedKey)}, false);
        });
        connect(erase, &QPushButton::clicked, this, [this, trashedKey, title] {
            QMessageBox box(QMessageBox::Warning, tr("Delete Permanently"),
                            tr("Permanently delete \"%1\", with its attachments and notes? This cannot be undone.")
                                .arg(title),
                            QMessageBox::NoButton, this);
            box.setObjectName("confirmDeletePermanently");
            auto* yes = box.addButton(tr("Delete Permanently"), QMessageBox::DestructiveRole);
            box.setDefaultButton(box.addButton(QMessageBox::Cancel));
            box.exec();
            if (box.clickedButton() == static_cast<QAbstractButton*>(yes))
                AppContext::get().library().eraseItems(libraryId_, {ss(trashedKey)});
        });
        lines_->addWidget(row);
    }
    if (isVisible()) {  // shown now, not a frame later (see InfoEditor::settleLayout)
        for (int i = 0; i < lines_->count(); ++i)
            if (auto* w = lines_->itemAt(i)->widget())
                w->show();
        lines_->activate();
    }
}

// --- ItemPane ----------------------------------------------------------------------------------

ItemPane::ItemPane(QWidget* parent) : QStackedWidget(parent)
{
    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setForegroundRole(QPalette::PlaceholderText);
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
    // Info and the item's notes side by side as tabs; clicking a paper that has notes
    // shows them directly unless the user last chose the Info tab.
    // the item's place in the library sits above both tabs
    itemContainer_ = new QWidget(this);
    auto* icv = new QVBoxLayout(itemContainer_);
    icv->setContentsMargins(0, 0, 0, 0);
    icv->setSpacing(0);
    locations_ << new LocationBox(itemContainer_);
    locations_.last()->setContentsMargins(10, 8, 10, 2);
    icv->addWidget(locations_.last());
    itemPage_ = new QTabWidget(itemContainer_);
    itemPage_->setDocumentMode(true);
    itemPage_->addTab(scrollPage(itemContent, itemPage_), tr("Info"));
    itemNotes_ = new ItemNotesPanel(itemPage_);
    itemPage_->addTab(itemNotes_, icon("type-note"), tr("Notes"));
    icv->addWidget(itemPage_, 1);
    addWidget(itemContainer_);
    connect(itemNotes_, &ItemNotesPanel::countChanged, this, [this](int n) {
        itemPage_->setTabText(1, n ? tr("Notes (%1)").arg(n) : tr("Notes"));
    });
    shareInfoNotesTab(itemPage_);

    // note
    notePage_ = new QWidget(this);
    auto* nv = new QVBoxLayout(notePage_);
    nv->setContentsMargins(0, 0, 0, 0);
    locations_ << new LocationBox(notePage_);
    locations_.last()->setContentsMargins(10, 6, 10, 0);
    nv->addWidget(locations_.last());
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
    locations_ << new LocationBox(attContent);
    av->addWidget(locations_.last());
    attachment_ = new AttachmentBox(attContent);
    av->addWidget(attachment_);
    attachmentTags_ = new TagsBox(attContent);
    av->addWidget(attachmentTags_);
    av->addStretch();
    attachmentPage_ = scrollPage(attContent, this);
    addWidget(attachmentPage_);

    connect(attachment_, &AttachmentBox::openRequested, this, &ItemPane::openAttachment);
    for (auto* location : locations_)
        connect(location, &LocationBox::scopeRequested, this,
                [this, location](const Scope& s) { emit showScope(s, currentKey_); });
    connect(related_, &RelatedBox::itemActivated, this, &ItemPane::selectItem);
    connect(noteRelated_, &RelatedBox::itemActivated, this, &ItemPane::selectItem);
    showItems(0, {});
}

void ItemPane::showInfoOrNotes(int tab) { chooseInfoNotesTab(itemPage_, tab); }

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
    currentKey_ = keys[0];
    for (auto* location : locations_)
        location->setItem(libraryId, keys[0]);
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
        itemNotes_->setItem(libraryId, keys[0]);
        int n = itemNotes_->count();
        itemPage_->setTabText(1, n ? tr("Notes (%1)").arg(n) : tr("Notes"));
        setCurrentWidget(itemContainer_);
    }
    // The page laid out around its new contents before it is drawn: the location's lines
    // above, the fields, and the scroll area's page as tall as they are (left to the
    // event loop, the fields were drawn squeezed into the old height for a frame). Each
    // round of layout can ask for another.
    for (int round = 0; round < 4; ++round)
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
}

} // namespace on::gui
