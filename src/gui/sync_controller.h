#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>

namespace on::sync {
class ChangeWatcher;
}

namespace on::gui {

class SyncHistory;

// Runs synchronisation with the active sync target on a background thread
// (with its own database connection) and schedules automatic syncs: after changes
// here, every few minutes, and (with automatic syncs on) as soon as the target tells
// of another computer's changes.
class SyncController : public QObject {
    Q_OBJECT
public:
    explicit SyncController(QObject* parent = nullptr);
    ~SyncController() override;

    bool isRunning() const { return running_; }
    bool hasTarget() const;
    bool isWatching() const { return watcher_ != nullptr; }  // for other computers' changes
    void reloadSettings();
    const SyncHistory& history() const { return *history_; }

public slots:
    void syncNow();
    void cancel();
    // Download one attachment file from the sync target (on-demand file mode).
    void downloadFile(qint64 libraryId, const QString& key);
    // Delete an empty library on the sync target (not here): remoteLibraryDeleted tells how
    // it went.
    void deleteRemoteLibrary(qint64 libraryId);

signals:
    void started();
    void progress(const QString& message);
    void finished(bool ok, const QString& summary);
    void fileDownloaded(qint64 libraryId, const QString& key, bool ok, const QString& error);
    void remoteLibraryDeleted(qint64 libraryId, bool ok, const QString& error);
    void historyChanged();

private:
    void join();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    QTimer autoTimer_;
    QTimer intervalTimer_;
    bool pendingAfterRun_ = false;
    std::unique_ptr<SyncHistory> history_;
    std::atomic<uint64_t> syncCount_{0};  // syncs started and finished: odd while one runs
    std::unique_ptr<sync::ChangeWatcher> watcher_;
};

} // namespace on::gui
