#pragma once

#include <QDialog>

#include "core/sync/history.h"

class QTreeWidget;

namespace on::gui {

// The window that shows the sync history.
class SyncHistoryWindow : public QDialog {
    Q_OBJECT
public:
    explicit SyncHistoryWindow(QWidget* parent = nullptr);
    void reload(const sync::SyncHistory& history);

signals:
    void syncRequested();
    void itemRequested(qint64 libraryId, const QString& key);
    void annotationRequested(qint64 libraryId, const QString& attachmentKey, const QString& annotationKey);

private:
    QTreeWidget* tree_;
};

} // namespace on::gui
