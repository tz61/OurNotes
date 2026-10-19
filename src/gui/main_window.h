#pragma once

#include <QMainWindow>

class QLabel;
class QTabWidget;
class QToolButton;

namespace fp::gui {

class LibraryView;
class ReaderTab;
class SyncController;
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
    ReaderTab* findReader(int64_t libraryId, const QString& key) const;
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
    void showConflicts();
    void setStatus(const QString& text, int timeoutMs = 0);

    QTabWidget* tabs_;
    LibraryView* library_;
    QToolButton* syncButton_ = nullptr;
    QLabel* status_;
    SyncController* sync_ = nullptr;
    FulltextIndexer* indexer_ = nullptr;
};

} // namespace fp::gui
