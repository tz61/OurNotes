#pragma once

#include <QDialog>

#include "core/json.h"

class QGroupBox;
class QLabel;
class QTreeWidget;

namespace on::gui {

class FancyProgressBar;

// Tools → Storage and Sync Status: what OurNotes takes on this computer's disk (its
// database, full-text index and attachment files, each library's) and the room left; and
// how the sync target is doing: its version and API, its disk, what it holds of each of
// the user's libraries. Looked up in the background each time it is shown or refreshed.
class StorageWindow : public QDialog {
    Q_OBJECT
public:
    explicit StorageWindow(QWidget* parent = nullptr);
    void refresh();
    bool isLoading() const { return pending_ > 0; }

signals:
    void syncSettingsRequested();

private:
    struct Disk {
        QLabel* text;
        FancyProgressBar* bar;
    };
    Disk addDisk(QWidget* parent);
    static void showDisk(const Disk& disk, const Json& space);
    void showLocal(const Json& usage, const QString& error);
    void showTarget(const Json& status, const Json& info, const QString& error);

    QGroupBox* localBox_;
    QLabel* dataDir_;
    Disk localDisk_;
    QLabel* database_;
    QLabel* fulltext_;
    QLabel* files_;
    QTreeWidget* localLibraries_;

    QGroupBox* targetBox_;
    QLabel* targetState_;
    QWidget* targetDetails_;
    QLabel* server_;
    Disk targetDisk_;
    QLabel* targetDatabase_;
    QLabel* targetFiles_;
    QTreeWidget* targetLibraries_;

    int generation_ = 0;  // answers to an earlier refresh() are dropped
    int pending_ = 0;
};

} // namespace on::gui
