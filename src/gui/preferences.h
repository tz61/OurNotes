#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QSpinBox;
class QTabWidget;

namespace fp::gui {

class SyncController;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(SyncController* sync, QWidget* parent = nullptr);
    void showPage(int index);
    bool restartRequested() const { return restart_; }

signals:
    void importZoteroRequested();
    void rebuildIndexRequested();
    void conflictsRequested();

private:
    QWidget* generalPage();
    QWidget* syncPage();
    QWidget* advancedPage();
    QWidget* formulaColorRow(QWidget* page);
    void reloadTargets();
    void addServer();
    void addFolder();
    void save();

    SyncController* sync_;
    QTabWidget* tabs_;
    QListWidget* targets_;
    QCheckBox* autoSync_;
    QSpinBox* interval_;
    QComboBox* fileMode_;
    QComboBox* openPdf_;
    QCheckBox* subcollections_;
    QCheckBox* fulltext_;
    QComboBox* scale_;
    QComboBox* appearance_;
    QLabel* scaleHint_;
    QLabel* syncStatus_;
    bool restart_ = false;
};

} // namespace fp::gui
