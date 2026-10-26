#include "gui/sync_history.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/common.h"
#include "gui/icons.h"

namespace on::gui {

namespace {

enum Role { LibraryRole = Qt::UserRole, KeyRole, ParentRole, KindRole, ActionRole };

QString whenText(const std::string& iso)
{
    auto t = QDateTime::fromString(qs(iso), Qt::ISODate).toLocalTime();
    if (!t.isValid())
        return qs(iso);
    QLocale locale;
    auto days = t.date().daysTo(QDate::currentDate());
    QString time = locale.toString(t.time(), QLocale::ShortFormat);
    if (days == 0)
        return QObject::tr("Today %1").arg(time);
    if (days == 1)
        return QObject::tr("Yesterday %1").arg(time);
    return locale.toString(t, QLocale::ShortFormat);
}

QString kindText(const std::string& kind)
{
    if (kind == "note")
        return QObject::tr("Note");
    if (kind == "annotation")
        return QObject::tr("Annotation");
    if (kind == "attachment")
        return QObject::tr("Attachment");
    if (kind == "file")
        return QObject::tr("File");
    if (kind == "collection")
        return QObject::tr("Collection");
    if (kind == "search")
        return QObject::tr("Saved search");
    if (kind == "setting")
        return QObject::tr("Setting");
    auto label = schema::typeLabel(kind);
    return label.empty() ? QObject::tr("Item") : qs(label);
}

QIcon kindIcon(const std::string& kind)
{
    if (kind == "collection")
        return icon("folder");
    if (kind == "file")
        return icon("attach");
    if (kind == "search" || kind == "setting")
        return {};
    return itemTypeIcon(qs(kind));
}

QString actionText(const std::string& action)
{
    if (action == "added")
        return QObject::tr("Added");
    if (action == "deleted")
        return QObject::tr("Deleted");
    return QObject::tr("Changed");
}

} // namespace

SyncHistoryWindow::SyncHistoryWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Sync History"));
    resize(720, 520);
    auto* v = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("What the last syncs brought in (↓) and sent out (↑). Something changed in several "
                               "syncs is listed once, under the last. Double-click to show it."),
                            this);
    hint->setWordWrap(true);
    v->addWidget(hint);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName("syncHistory");
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({tr("What"), tr("Change"), tr("By"), tr("Library")});
    tree_->headerItem()->setToolTip(2, tr("The program that made a change sent from here, when it was not OurNotes: "
                                          "ournotes-cli, which AI agents and scripts use"));
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    v->addWidget(tree_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* sync = buttons->addButton(tr("Sync Now"), QDialogButtonBox::ActionRole);
    connect(sync, &QPushButton::clicked, this, &SyncHistoryWindow::syncRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        auto key = item->data(0, KeyRole).toString();
        if (key.isEmpty() || item->data(0, ActionRole).toString() == "deleted")
            return;
        qint64 lib = item->data(0, LibraryRole).toLongLong();
        auto kind = item->data(0, KindRole).toString();
        if (kind == "annotation")
            emit annotationRequested(lib, item->data(0, ParentRole).toString(), key);
        else if (kind != "collection" && kind != "search" && kind != "setting")
            emit itemRequested(lib, key);
    });
}

void SyncHistoryWindow::reload(const sync::SyncHistory& history)
{
    tree_->clear();
    int shown = 0;
    bool byOthers = false;  // the By column is only shown when needed
    for (auto& run : history.runs()) {
        auto* head = new QTreeWidgetItem(tree_);
        auto& changes = run["changes"];
        int in = 0, out = 0;
        for (auto& c : changes)
            (c.value("incoming", false) ? in : out)++;
        QString what;
        if (!run.value("ok", true)) {
            auto errors = run.value("errors", Json::array());
            what = tr("Failed: %1").arg(errors.empty() ? tr("unknown error") : qs(errors[0].get<std::string>()));
            head->setForeground(0, QColor(0xc0, 0x39, 0x2b));
        } else if (changes.empty() && run.value("more", 0) == 0) {
            what = tr("No changes");
        } else {
            QStringList parts;
            if (in)
                parts << tr("↓ %n received", nullptr, in);
            if (out)
                parts << tr("↑ %n sent", nullptr, out);
            if (int more = run.value("more", 0))
                parts << tr("%n more", nullptr, more);
            what = parts.join("  ");
        }
        auto target = jqs(run, "target");
        auto by = jqs(run, "by");
        head->setText(0, whenText(jstr(run, "time")) + (target.isEmpty() ? QString() : "  ·  " + target) + "  ·  " +
                             what + (by.isEmpty() ? QString() : "  ·  " + tr("synced by %1").arg(by)));
        head->setFirstColumnSpanned(true);
        QFont bold = head->font(0);
        bold.setBold(true);
        head->setFont(0, bold);
        for (auto& c : changes) {
            auto* row = new QTreeWidgetItem(head);
            auto kind = jstr(c, "kind");
            auto title = jqs(c, "title");
            QString arrow = c.value("incoming", false) ? "↓ " : "↑ ";
            row->setText(0, arrow + kindText(kind) + (title.isEmpty() ? QString() : "  “" + title + "”"));
            row->setToolTip(0, plainTip(title));
            row->setIcon(0, kindIcon(kind));
            row->setText(1, actionText(jstr(c, "action")));
            row->setText(2, jqs(c, "by"));
            byOthers = byOthers || !row->text(2).isEmpty();
            row->setText(3, jqs(c, "library"));
            row->setData(0, LibraryRole, (qint64)c.value("libraryId", int64_t(0)));
            row->setData(0, KeyRole, jqs(c, "key"));
            row->setData(0, ParentRole, jqs(c, "parentKey"));
            row->setData(0, KindRole, qs(kind));
            row->setData(0, ActionRole, jqs(c, "action"));
        }
        head->setExpanded(shown++ < 3);
    }
    tree_->setColumnHidden(2, !byOthers);
    if (!tree_->topLevelItemCount()) {
        auto* none = new QTreeWidgetItem(tree_);
        none->setText(0, tr("No syncs yet."));
        none->setFirstColumnSpanned(true);
    }
}

} // namespace on::gui
