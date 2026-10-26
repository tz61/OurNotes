#include "gui/sync_controller.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <map>
#include <optional>

#include "core/sync/engine.h"
#include "core/sync/history.h"
#include "ournotes/version.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "sync/change_watcher.h"
#include "sync/http_transport.h"
#include "sync/local_transport.h"

namespace on::gui {

std::unique_ptr<sync::Transport> makeTransport(const SyncTarget& t)
{
    if (t.kind == "local")
        return std::make_unique<sync::LocalTransport>(u8path(t.path));
    return std::make_unique<sync::HttpTransport>(t.url, t.token, t.verifyTls);
}

SyncController::SyncController(QObject* parent)
    : QObject(parent), history_(std::make_unique<sync::SyncHistory>(AppContext::get().library().dataDir() / "sync-history.json"))
{
    autoTimer_.setSingleShot(true);
    autoTimer_.setInterval(20000);
    connect(&autoTimer_, &QTimer::timeout, this, &SyncController::syncNow);
    // what ournotes-cli or an agent changed while the app runs goes out like the app's own
    connect(&AppContext::get(), &AppContext::changedElsewhere, this, [this] {
        if (AppContext::get().config().get("/sync/auto", true).get<bool>() && hasTarget())
            autoTimer_.start();
    });
    // and when ournotes-cli synced, the history has its sync
    historyTimer_.setInterval(2000);
    connect(&historyTimer_, &QTimer::timeout, this, [this] {
        if (history_->reload())
            emit historyChanged();
    });
    historyTimer_.start();
    connect(&intervalTimer_, &QTimer::timeout, this, &SyncController::syncNow);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString& action, const QStringList&) {
                if (action == "refresh" || (type == "library" && action != "modify"))
                    return;  // a rename is synced, the rest is the library's own state
                if (AppContext::get().config().get("/sync/auto", true).get<bool>() && hasTarget())
                    autoTimer_.start();
            });
    reloadSettings();
}

SyncController::~SyncController()
{
    watcher_.reset();
    cancel_ = true;
    join();
}

void SyncController::join()
{
    if (thread_.joinable())
        thread_.join();
}

bool SyncController::hasTarget() const { return AppContext::get().config().activeTarget().has_value(); }

void SyncController::reloadSettings()
{
    auto& cfg = AppContext::get().config();
    int minutes = cfg.get("/sync/intervalMinutes", 15).get<int>();
    if (minutes > 0 && hasTarget())
        intervalTimer_.start(minutes * 60 * 1000);
    else
        intervalTimer_.stop();

    watcher_.reset();
    auto target = cfg.activeTarget();
    if (!target || !cfg.get("/sync/auto", true).get<bool>())
        return;
    sync::ChangeWatcher::Hooks hooks;
    hooks.connect = [t = *target] { return makeTransport(t); };
    // with a database connection of its own, on the watcher's thread
    hooks.pulledVersions = [dataDir = AppContext::get().library().dataDir(), id = target->id,
                            lib = std::shared_ptr<Library>()]() mutable {
        if (!lib)
            lib = std::make_shared<Library>(dataDir);
        std::map<std::string, int64_t> pulled;
        for (auto& l : lib->libraries())
            if (l.syncTarget == id)
                pulled[l.uid] = l.syncVersion;
        return pulled;
    };
    hooks.syncCount = [this] { return syncCount_.load(); };
    hooks.changed = [this] {
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (hasTarget())
                    syncNow();
            },
            Qt::QueuedConnection);
    };
    watcher_ = std::make_unique<sync::ChangeWatcher>(std::move(hooks));
}

void SyncController::cancel() { cancel_ = true; }

void SyncController::syncNow()
{
    auto target = AppContext::get().config().activeTarget();
    if (!target) {
        emit finished(false, tr("No sync target is configured."));
        return;
    }
    if (running_) {
        pendingAfterRun_ = true;
        return;
    }
    join();
    autoTimer_.stop();
    running_ = true;
    ++syncCount_;
    AppContext::get().beginOwnWrites();
    cancel_ = false;
    pendingAfterRun_ = false;
    emit started();
    auto dataDir = AppContext::get().library().dataDir();
    bool downloadFiles = AppContext::get().config().get("/sync/files", "on_sync").get<std::string>() != "on_demand";
    thread_ = std::thread([this, dataDir, t = *target, downloadFiles] {
        bool ok = true;
        QString summary;
        std::optional<sync::SyncReport> report;
        try {
            Library lib(dataDir);
            auto transport = makeTransport(t);
            sync::SyncOptions opts;
            opts.downloadFiles = downloadFiles;
            opts.cancelled = [this] { return cancel_.load(); };
            opts.log = [this](const std::string& m) {
                QMetaObject::invokeMethod(this, [this, msg = qs(m)] { emit progress(msg); }, Qt::QueuedConnection);
            };
            opts.progress = [this](const std::string& stage, int done, int total) {
                auto msg = total > 0 ? QString("%1 (%2/%3)").arg(qs(stage)).arg(done).arg(total) : qs(stage);
                QMetaObject::invokeMethod(this, [this, msg] { emit progress(msg); }, Qt::QueuedConnection);
            };
            sync::SyncEngine engine(lib, *transport, t.id, opts);
            report = engine.syncAll();
            summary = qs(report->summary());
            ok = report->errors.empty();
            if (!ok)
                summary += "\n" + qs(on::join(report->errors, "\n"));
        } catch (const sync::SyncError& e) {
            ok = false;
            summary = e.status == 401 ? tr("Authentication failed. Sign in again in Preferences → Sync.") : qs(e.what());
        } catch (const std::exception& e) {
            ok = false;
            summary = qs(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, summary, report = std::move(report), target = t.name] {
                if (report)
                    history_->add(target, *report, ok, ss(summary.section('\n', 0, 0)));
                else
                    history_->addFailure(target, ss(summary));
                emit historyChanged();
                running_ = false;
                ++syncCount_;
                AppContext::get().endOwnWrites();
                AppContext::get().library().notifyRefresh();
                emit AppContext::get().syncFinished();
                emit finished(ok, summary);
                if (pendingAfterRun_)
                    autoTimer_.start(2000);
            },
            Qt::QueuedConnection);
    });
}

void SyncController::downloadFile(qint64 libraryId, const QString& key)
{
    auto target = AppContext::get().config().activeTarget();
    if (!target) {
        emit fileDownloaded(libraryId, key, false, tr("No sync target is configured."));
        return;
    }
    auto dataDir = AppContext::get().library().dataDir();
    AppContext::get().beginOwnWrites();
    // Detached, the thread can outlive this controller (the window closed during a
    // download): the result goes through the application, and here only if still here.
    QPointer<SyncController> self(this);
    std::thread([self, dataDir, t = *target, libraryId, key] {
        bool ok = false;
        QString error;
        try {
            Library lib(dataDir);
            auto transport = makeTransport(t);
            sync::SyncEngine engine(lib, *transport, t.id);
            ok = engine.downloadAttachment(libraryId, ss(key));
        } catch (const std::exception& e) {
            error = qs(e.what());
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, libraryId, key, ok, error] {
                if (!self)
                    return;
                AppContext::get().endOwnWrites();
                emit self->fileDownloaded(libraryId, key, ok, error);
            },
            Qt::QueuedConnection);
    }).detach();
}

void SyncController::deleteRemoteLibrary(qint64 libraryId)
{
    auto target = AppContext::get().config().activeTarget();
    auto info = AppContext::get().library().library(libraryId);
    if (!target || !info) {
        emit remoteLibraryDeleted(libraryId, false, tr("No sync target is configured."));
        return;
    }
    auto dataDir = AppContext::get().library().dataDir();
    AppContext::get().beginOwnWrites();
    QPointer<SyncController> self(this);  // (see downloadFile)
    std::thread([self, dataDir, t = *target, libraryId, uid = info->uid] {
        bool ok = false;
        QString error;
        try {
            Library lib(dataDir);
            auto transport = makeTransport(t);
            sync::SyncEngine(lib, *transport, t.id).deleteRemoteLibrary(uid);
            ok = true;
        } catch (const sync::SyncError& e) {
            error = e.status == 401 ? tr("Authentication failed. Sign in again in Preferences → Sync.") : qs(e.what());
        } catch (const std::exception& e) {
            error = qs(e.what());
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, libraryId, ok, error] {
                if (!self)
                    return;
                AppContext::get().endOwnWrites();
                emit self->remoteLibraryDeleted(libraryId, ok, error);
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace on::gui
