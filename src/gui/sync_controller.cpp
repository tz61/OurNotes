#include "gui/sync_controller.h"

#include <QMetaObject>

#include "core/sync/engine.h"
#include "ournotes/version.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "sync/http_transport.h"
#include "sync/local_transport.h"

namespace on::gui {

namespace {

std::unique_ptr<sync::Transport> makeTransport(const SyncTarget& t)
{
    if (t.kind == "local")
        return std::make_unique<sync::LocalTransport>(u8path(t.path));
    return std::make_unique<sync::HttpTransport>(t.url, t.token, t.verifyTls);
}

} // namespace

SyncController::SyncController(QObject* parent) : QObject(parent)
{
    autoTimer_.setSingleShot(true);
    autoTimer_.setInterval(20000);
    connect(&autoTimer_, &QTimer::timeout, this, &SyncController::syncNow);
    connect(&intervalTimer_, &QTimer::timeout, this, &SyncController::syncNow);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString& action, const QStringList&) {
                if (action == "refresh" || type == "library")
                    return;
                if (AppContext::get().config().get("/sync/auto", true).get<bool>() && hasTarget())
                    autoTimer_.start();
            });
    reloadSettings();
}

SyncController::~SyncController()
{
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
    int minutes = AppContext::get().config().get("/sync/intervalMinutes", 15).get<int>();
    if (minutes > 0 && hasTarget())
        intervalTimer_.start(minutes * 60 * 1000);
    else
        intervalTimer_.stop();
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
    cancel_ = false;
    pendingAfterRun_ = false;
    emit started();
    auto dataDir = AppContext::get().library().dataDir();
    bool downloadFiles = AppContext::get().config().get("/sync/files", "on_sync").get<std::string>() != "on_demand";
    thread_ = std::thread([this, dataDir, t = *target, downloadFiles] {
        bool ok = true;
        QString summary;
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
            auto report = engine.syncAll();
            summary = qs(report.summary());
            ok = report.errors.empty();
            if (!ok)
                summary += "\n" + qs(on::join(report.errors, "\n"));
        } catch (const sync::SyncError& e) {
            ok = false;
            summary = e.status == 401 ? tr("Authentication failed. Sign in again in Preferences → Sync.") : qs(e.what());
        } catch (const std::exception& e) {
            ok = false;
            summary = qs(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, summary] {
                running_ = false;
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
    std::thread([this, dataDir, t = *target, libraryId, key] {
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
            this, [this, libraryId, key, ok, error] { emit fileDownloaded(libraryId, key, ok, error); },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace on::gui
