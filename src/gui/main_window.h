#pragma once

#include <QMainWindow>

#include <vector>

class QIcon;
class QLabel;
class QTabWidget;
class QToolButton;

namespace on::gui {

class LibraryView;
class ReaderTab;
class SyncController;
class SyncHistoryWindow;
class ConnectorServer;
class FulltextIndexer;
struct Scope;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void openAttachment(int64_t libraryId, const QString& key, int page = -1, const QString& annotation = {});
    void openFiles(const QStringList& files);
    void offerZoteroImport();
    void selectItem(const QString& key);
    void importZotero();

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void buildActions();
    int findTab(int64_t libraryId, const QString& key) const;  // a reader, loaded or not
    void reopenClosedTab();
    QIcon readerIcon(int64_t libraryId, const QString& key) const;
    struct ClosedTab {
        int64_t libraryId;
        QString key;
        int index;
    };
    std::vector<ClosedTab> closedTabs_;  // newest last; this window's only, not kept
    // Open tabs and the library's view are saved on quit and restored on start; restored
    // tabs load when they are first shown.
    void saveSession();
    void restoreSession();
    void loadPendingTab(int index);
    void shutDown();
    void updateTabActions();
    void showPreferences(int page = 0);
    void restartApplication();
    void showAbout();
    void exportItems(const Scope& scope, const QStringList& keys);
    void importFile();
    void createBibliography(const Scope& scope, const QStringList& keys);
    void copyCitation(bool bibliography);
    void addByIdentifier();
    void retrieveMetadata(int64_t libraryId, const QStringList& keys);
    void newLibrary();
    // Only an empty library: on the sync target first, when it is synced there.
    void deleteLibrary(int64_t libraryId);
    void showConflicts();
    void showSyncHistory();
    void setStatus(const QString& text, int timeoutMs = 0);

    QTabWidget* tabs_;
    LibraryView* library_;
    QToolButton* syncButton_ = nullptr;
    SyncHistoryWindow* syncHistory_ = nullptr;
    ConnectorServer* connector_ = nullptr;
    void startConnector(bool on);
    QLabel* status_;
    SyncController* sync_ = nullptr;
    FulltextIndexer* indexer_ = nullptr;
    bool shutDown_ = false;
};

} // namespace on::gui
