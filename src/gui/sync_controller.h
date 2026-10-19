#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>

namespace on::gui {

// Runs synchronisation with the active sync target on a background thread
// (with its own database connection) and schedules automatic syncs.
class SyncController : public QObject {
    Q_OBJECT
public:
    explicit SyncController(QObject* parent = nullptr);
    ~SyncController() override;

    bool isRunning() const { return running_; }
    bool hasTarget() const;
    void reloadSettings();

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

private:
    void join();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    QTimer autoTimer_;
    QTimer intervalTimer_;
    bool pendingAfterRun_ = false;
};

} // namespace on::gui
