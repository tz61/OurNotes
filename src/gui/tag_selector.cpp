#include "gui/tag_selector.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"

namespace on::gui {

TagSelector::TagSelector(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    list_ = new QListWidget(this);
    list_->setFlow(QListView::LeftToRight);
    list_->setWrapping(true);
    list_->setResizeMode(QListView::Adjust);
    list_->setSpacing(1);
    list_->setSelectionMode(QAbstractItemView::MultiSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setUniformItemSizes(false);
    layout->addWidget(list_, 1);

    auto* bottom = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter tags"));
    filter_->setClearButtonEnabled(true);
    deselect_ = new QToolButton(this);
    deselect_->setText(tr("Deselect All"));
    deselect_->setAutoRaise(true);
    bottom->addWidget(filter_, 1);
    bottom->addWidget(deselect_);
    layout->addLayout(bottom);

    connect(list_, &QListWidget::itemSelectionChanged, this, [this] { emit selectionChanged(selectedTags()); });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < list_->count(); ++i) {
            auto* it = list_->item(i);
            it->setHidden(!text.isEmpty() && !it->text().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(deselect_, &QToolButton::clicked, list_, &QListWidget::clearSelection);
    connect(list_, &QWidget::customContextMenuRequested, this, &TagSelector::showMenu);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList& keys) {
                // of the settings, only tag colours matter (not e.g. reading positions)
                bool colors = type == "setting" && keys.contains("tagColors");
                if ((lib == libraryId_ || lib == 0) && (type == "item" || colors || type == "*"))
                    reload();
            });
}

void TagSelector::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        QMetaObject::invokeMethod(this, &TagSelector::reload, Qt::QueuedConnection);
}

void TagSelector::setLibrary(int64_t libraryId)
{
    if (libraryId == libraryId_)
        return;
    libraryId_ = libraryId;
    QSignalBlocker block(list_);
    list_->clear();
    reload();
    emit selectionChanged({});
}

QStringList TagSelector::selectedTags() const
{
    QStringList out;
    for (auto* it : list_->selectedItems())
        out << it->data(Qt::UserRole).toString();
    return out;
}

void TagSelector::reload()
{
    auto selected = selectedTags();
    QSignalBlocker block(list_);
    list_->clear();
    if (!libraryId_)
        return;
    auto tags = AppContext::get().library().tags(libraryId_);
    std::stable_sort(tags.begin(), tags.end(), [](const TagInfo& a, const TagInfo& b) {
        bool ca = !a.color.empty(), cb = !b.color.empty();
        if (ca != cb)
            return ca;
        if (ca)
            return a.position < b.position;
        return normalize(a.name) < normalize(b.name);
    });
    for (auto& t : tags) {
        auto* item = new QListWidgetItem(qs(t.name), list_);
        item->setData(Qt::UserRole, qs(t.name));
        item->setToolTip(tr("%n item(s)", nullptr, t.count));
        if (!t.color.empty())
            item->setIcon(colorIcon(qs(t.color), 10));
        if (t.type == 1) {
            QFont f = item->font();
            f.setItalic(true);
            item->setFont(f);
        }
        if (t.count == 0)
            item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        if (selected.contains(qs(t.name)))
            item->setSelected(true);
        if (!filter_->text().isEmpty() && !item->text().contains(filter_->text(), Qt::CaseInsensitive))
            item->setHidden(true);
    }
    block.unblock();
    if (selectedTags() != selected)
        emit selectionChanged(selectedTags());
}

void TagSelector::showMenu(const QPoint& pos)
{
    auto* item = list_->itemAt(pos);
    if (!item || !libraryId_)
        return;
    auto tag = item->data(Qt::UserRole).toString();
    auto& lib = AppContext::get().library();
    bool editable = AppContext::get().editable(libraryId_);
    QMenu menu(this);
    auto* colors = menu.addMenu(tr("Assign Colour"));
    colors->setEnabled(editable);
    for (auto& c : schema::kAnnotationColors)
        colors->addAction(colorIcon(c.hex), tr(c.name), this,
                          [&, hex = std::string(c.hex)] { lib.setTagColor(libraryId_, ss(tag), hex); });
    colors->addSeparator();
    colors->addAction(tr("Remove Colour"), this, [&] { lib.setTagColor(libraryId_, ss(tag), ""); });
    menu.addAction(tr("Rename Tag…"), this, [&] {
        bool ok = false;
        auto name = QInputDialog::getText(this, tr("Rename Tag"), tr("New name:"), QLineEdit::Normal, tag, &ok);
        if (ok && !name.trimmed().isEmpty())
            lib.renameTag(libraryId_, ss(tag), ss(name.trimmed()));
    })->setEnabled(editable);
    menu.addAction(tr("Delete Tag…"), this, [&] {
        if (QMessageBox::question(this, tr("Delete Tag"), tr("Remove the tag \"%1\" from all items?").arg(tag)) ==
            QMessageBox::Yes)
            lib.deleteTag(libraryId_, ss(tag));
    })->setEnabled(editable);
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

} // namespace on::gui
