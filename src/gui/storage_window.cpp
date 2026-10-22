#include "gui/storage_window.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <thread>

#include "core/library.h"
#include "core/sync/protocol.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/fancy_progress.h"
#include "gui/sync_controller.h"

namespace on::gui {

namespace {

QString bytes(const Json& j, const char* key = nullptr)
{
    auto v = key ? j.value(key, Json(0)) : j;
    return QLocale().formattedDataSize(v.is_number() ? v.get<int64_t>() : 0, 1, QLocale::DataSizeTraditionalFormat);
}

QString number(const Json& j, const char* key)
{
    auto v = j.value(key, Json(0));
    return QLocale().toString((qlonglong)(v.is_number() ? v.get<int64_t>() : 0));
}

QString files(const Json& j, const char* key)
{
    auto v = j.value(key, Json(0));
    auto n = v.is_number() ? v.get<int64_t>() : 0;
    return n == 1 ? StorageWindow::tr("1 file") : StorageWindow::tr("%1 files").arg(QLocale().toString((qlonglong)n));
}

QString runningFor(int64_t seconds)
{
    if (seconds >= 2 * 86400)
        return StorageWindow::tr("%1 days").arg(seconds / 86400);
    if (seconds >= 3600)
        return StorageWindow::tr("%1 h %2 min").arg(seconds / 3600).arg(seconds % 3600 / 60);
    return StorageWindow::tr("%1 min").arg(std::max<int64_t>(seconds / 60, 1));
}

QTreeWidget* newTable(const QStringList& columns, QWidget* parent)
{
    auto* t = new QTreeWidget(parent);
    t->setHeaderLabels(columns);
    t->setRootIsDecorated(false);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setFocusPolicy(Qt::NoFocus);
    t->header()->setStretchLastSection(false);
    t->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    t->setMinimumHeight(t->fontMetrics().height() * 5);
    return t;
}

void alignNumbers(QTreeWidgetItem* row, int from)
{
    for (int c = from; c < row->columnCount(); ++c)
        row->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
}

} // namespace

StorageWindow::StorageWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Storage and Sync Status"));
    setObjectName("storageWindow");
    resize(760, 680);
    auto* v = new QVBoxLayout(this);

    // this computer
    localBox_ = new QGroupBox(tr("This computer"), this);
    auto* lv = new QVBoxLayout(localBox_);
    auto* lf = new QFormLayout;
    dataDir_ = new QLabel(localBox_);
    dataDir_->setObjectName("dataDir");
    dataDir_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(dataDir_, &QLabel::linkActivated, this, [](const QString& link) { QDesktopServices::openUrl(QUrl(link)); });
    lf->addRow(tr("Data folder:"), dataDir_);
    localDisk_ = addDisk(localBox_);
    localDisk_.text->setObjectName("localDisk");
    lf->addRow(tr("Disk:"), localDisk_.bar);
    lf->addRow(QString(), localDisk_.text);
    database_ = new QLabel(localBox_);
    lf->addRow(tr("Library database:"), database_);
    fulltext_ = new QLabel(localBox_);
    lf->addRow(tr("Full-text index:"), fulltext_);
    files_ = new QLabel(localBox_);
    files_->setObjectName("localFiles");
    lf->addRow(tr("Attachment files:"), files_);
    lv->addLayout(lf);
    localLibraries_ = newTable({tr("Library"), tr("Items"), tr("Notes"), tr("Annotations"), tr("Files"), tr("Size")}, localBox_);
    localLibraries_->setObjectName("localLibraries");
    lv->addWidget(localLibraries_, 1);
    v->addWidget(localBox_, 1);

    // the sync target
    targetBox_ = new QGroupBox(tr("Sync"), this);
    auto* tv = new QVBoxLayout(targetBox_);
    targetState_ = new QLabel(targetBox_);
    targetState_->setObjectName("targetState");
    targetState_->setWordWrap(true);
    targetState_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tv->addWidget(targetState_);
    targetDetails_ = new QWidget(targetBox_);
    auto* dv = new QVBoxLayout(targetDetails_);
    dv->setContentsMargins(0, 0, 0, 0);
    auto* tf = new QFormLayout;
    server_ = new QLabel(targetDetails_);
    server_->setObjectName("server");
    server_->setWordWrap(true);
    server_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tf->addRow(tr("Server:"), server_);
    targetDisk_ = addDisk(targetDetails_);
    targetDisk_.text->setObjectName("targetDisk");
    tf->addRow(tr("Disk:"), targetDisk_.bar);
    tf->addRow(QString(), targetDisk_.text);
    targetDatabase_ = new QLabel(targetDetails_);
    tf->addRow(tr("Database:"), targetDatabase_);
    targetFiles_ = new QLabel(targetDetails_);
    targetFiles_->setObjectName("targetFiles");
    tf->addRow(tr("Attachment files:"), targetFiles_);
    dv->addLayout(tf);
    targetLibraries_ = newTable({tr("Library"), tr("Access"), tr("Version"), tr("Items"), tr("Notes"),
                                 tr("Annotations"), tr("Files"), tr("Size")},
                                targetDetails_);
    targetLibraries_->setObjectName("targetLibraries");
    dv->addWidget(targetLibraries_, 1);
    tv->addWidget(targetDetails_, 1);
    v->addWidget(targetBox_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* settings = buttons->addButton(tr("Sync Settings…"), QDialogButtonBox::HelpRole);  // (on the left)
    connect(settings, &QPushButton::clicked, this, &StorageWindow::syncSettingsRequested);
    auto* again = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    connect(again, &QPushButton::clicked, this, &StorageWindow::refresh);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);
}

StorageWindow::Disk StorageWindow::addDisk(QWidget* parent)
{
    Disk d{new QLabel(parent), new FancyProgressBar(parent)};
    d.bar->setMaximum(1000);
    d.bar->setState(FancyProgressBar::State::Done);
    return d;
}

void StorageWindow::showDisk(const Disk& disk, const Json& space)
{
    int64_t capacity = space.is_object() ? space.value("capacity", Json(0)).get<int64_t>() : 0;
    if (capacity <= 0) {
        disk.bar->setValue(0);
        disk.text->setText(tr("Its size cannot be told."));
        return;
    }
    int64_t available = space.value("available", Json(0)).get<int64_t>();
    int64_t used = capacity - space.value("free", Json(0)).get<int64_t>();
    int permille = int(used * 1000 / capacity);
    disk.bar->setValue(permille);
    disk.bar->setState(permille > 900 ? FancyProgressBar::State::Warning : FancyProgressBar::State::Done);
    disk.text->setText(tr("%1 free of %2 (%3% used)").arg(bytes(available), bytes(capacity)).arg((permille + 5) / 10));
}

void StorageWindow::refresh()
{
    int generation = ++generation_;
    pending_ = 1;
    database_->setText(tr("Counting…"));
    QPointer<StorageWindow> self(this);
    auto dataDir = AppContext::get().library().dataDir();
    std::thread([self, generation, dataDir] {
        Json usage;
        QString error;
        try {
            Library lib(dataDir);  // a connection of its own, on this thread
            usage = lib.diskUsage();
        } catch (const std::exception& e) {
            error = qs(e.what());
        }
        QMetaObject::invokeMethod(qApp, [self, generation, usage, error] {
            if (self && self->generation_ == generation) {
                --self->pending_;
                self->showLocal(usage, error);
            }
        });
    }).detach();

    targetState_->show();
    auto target = AppContext::get().config().activeTarget();
    if (!target) {
        targetBox_->setTitle(tr("Sync"));
        targetState_->setText(tr("No sync target is set up: libraries stay on this computer."));
        targetDetails_->hide();
        return;
    }
    bool folder = target->kind == "local";
    targetBox_->setTitle(folder ? tr("Sync folder: %1").arg(qs(target->name)) : tr("Sync server: %1").arg(qs(target->name)));
    targetState_->setText(folder ? tr("Looking at the folder…") : tr("Asking the server…"));
    targetDetails_->hide();
    ++pending_;
    std::thread([self, generation, t = *target] {
        Json status, info;
        QString error;
        try {
            auto transport = makeTransport(t);
            if (auto s = transport->status())
                status = *s;
            else
                info = transport->info();
        } catch (const sync::SyncError& e) {
            error = e.status == 401 ? tr("Not signed in (sign in again in Sync Settings).") : qs(e.what());
        } catch (const std::exception& e) {
            error = qs(e.what());
        }
        QMetaObject::invokeMethod(qApp, [self, generation, status, info, error] {
            if (self && self->generation_ == generation) {
                --self->pending_;
                self->showTarget(status, info, error);
            }
        });
    }).detach();
}

void StorageWindow::showLocal(const Json& usage, const QString& error)
{
    localLibraries_->clear();
    if (!error.isEmpty()) {
        database_->setText(tr("Could not be counted: %1").arg(error));
        return;
    }
    auto dir = jqs(usage, "dataDir");
    dataDir_->setText(QString("<a href=\"%1\">%2</a>")
                          .arg(QUrl::fromLocalFile(dir).toString().toHtmlEscaped(), dir.toHtmlEscaped()));
    showDisk(localDisk_, usage.value("disk", Json()));
    database_->setText(bytes(usage, "database"));
    fulltext_->setText(bytes(usage, "fulltextIndex"));
    int64_t missing = 0;
    for (auto& l : usage.value("libraries", Json::array()))
        missing += l.value("missingFiles", Json(0)).get<int64_t>();
    auto storage = usage.value("storage", Json::object());
    auto text = tr("%1, %2").arg(files(storage, "files"), bytes(storage, "bytes"));
    if (missing > 0)
        text += tr(" (%1 more not downloaded)").arg(missing);
    files_->setText(text);
    for (auto& l : usage.value("libraries", Json::array())) {
        auto files = number(l, "files");
        if (l.value("missingFiles", Json(0)).get<int64_t>() > 0)
            files = tr("%1 (+%2)").arg(files, number(l, "missingFiles"));
        auto* row = new QTreeWidgetItem(localLibraries_, {jqs(l, "name"), number(l, "items"), number(l, "notes"),
                                                          number(l, "annotations"), files, bytes(l, "fileBytes")});
        alignNumbers(row, 1);
        row->setToolTip(0, tr("%1 in the trash").arg(number(l, "trashed")));
        if (l.value("missingFiles", Json(0)).get<int64_t>() > 0)
            row->setToolTip(4, tr("(+n): files not downloaded to this computer"));
    }
}

void StorageWindow::showTarget(const Json& status, const Json& info, const QString& error)
{
    targetLibraries_->clear();
    if (!error.isEmpty()) {
        targetState_->setText(tr("Could not reach it: %1").arg(error));
        targetDetails_->hide();
        return;
    }
    if (status.is_null()) {  // a server before 0.1.11
        targetState_->setText(tr("%1 %2, sync API %3. This server does not tell its storage: version 0.1.11 or later does.")
                                  .arg(jqs(info, "server"), jqs(info, "version"))
                                  .arg(info.value("api", Json(0)).get<int>()));
        targetDetails_->hide();
        return;
    }
    targetState_->clear();
    targetState_->setVisible(false);
    bool folder = jstr(status, "server") == "local";
    auto api = status.value("api", Json(0)).get<int>();
    server_->setText(folder ? tr("a folder, %1 (sync API %2)").arg(jqs(status, "path")).arg(api)
                            : tr("%1 %2, sync API %3, running for %4")
                                  .arg(jqs(status, "server"), jqs(status, "version"))
                                  .arg(api)
                                  .arg(runningFor(status.value("uptime", Json(0)).get<int64_t>())));
    showDisk(targetDisk_, status.value("disk", Json()));
    targetDatabase_->setText(bytes(status, "database"));
    auto stored = status.value("files", Json::object());
    targetFiles_->setText(tr("%1, %2 (everyone's)").arg(files(stored, "count"), bytes(stored, "bytes")));
    for (auto& l : status.value("libraries", Json::array())) {
        auto* row = new QTreeWidgetItem(targetLibraries_,
                                        {jqs(l, "name"), jqs(l, "role"), number(l, "version"), number(l, "items"),
                                         number(l, "notes"), number(l, "annotations"), number(l, "files"),
                                         bytes(l, "fileBytes")});
        alignNumbers(row, 2);
    }
    targetDetails_->show();
}

} // namespace on::gui
