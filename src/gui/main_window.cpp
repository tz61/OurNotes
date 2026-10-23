#include "gui/main_window.h"
#include "gui/metadata_progress.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QProgressDialog>
#include <QScreen>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QCursor>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <thread>

#include "core/schema.h"
#include "core/zotero_importer.h"
#include "ournotes/version.h"
#include "gui/app_context.h"
#include "gui/collection_tree.h"
#include "gui/common.h"
#include "gui/connector_server.h"
#include "gui/formats_actions.h"
#include "gui/fulltext_indexer.h"
#include "gui/icons.h"
#include "gui/library_view.h"
#include "gui/mac_fullscreen.h"
#include "gui/memory.h"
#include "gui/preferences.h"
#include "gui/reader/reader_tab.h"
#include "gui/sync_controller.h"
#include "gui/storage_window.h"
#include "gui/sync_history.h"
#include "gui/tab_widget.h"
#include "gui/x11_size_hints.h"

namespace on::gui {

namespace {

// A restored reader tab that has not been shown yet; becomes a ReaderTab when it is.
class PendingReaderTab : public QWidget {
public:
    PendingReaderTab(int64_t libraryId, const QString& key) : libraryId(libraryId), key(key)
    {
        label = new QLabel(this);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        (new QVBoxLayout(this))->addWidget(label);
    }
    const int64_t libraryId;
    const QString key;
    QLabel* label;
};

QString tabText(const QString& title) { return title.length() > 40 ? title.left(38) + "…" : title; }

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(OURNOTES_APP_NAME);
    setAcceptDrops(true);
    auto* tabs = new TabWidget(this);  // tabs wrap onto more rows when there are many
    tabs_ = tabs;
    tabs_->setObjectName("mainTabs");
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    library_ = new LibraryView(tabs_);
    tabs_->addTab(library_, icon("library"), tr("Library"));
    tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    tabs_->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);
    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(tabs->tabStrip());
    centralLayout->addWidget(tabs_, 1);
    setCentralWidget(central);
    layout()->setSizeConstraint(QLayout::SetNoConstraint);  // see followMinimumSize()
    connect(&titleBarTimer_, &QTimer::timeout, this, &MainWindow::followTitleBar);
    status_ = new QLabel(this);
    statusBar()->addWidget(status_, 1);

    sync_ = new SyncController(this);
    AppContext::get().watchDatabase();  // for what ournotes-cli or an agent changes meanwhile
    indexer_ = new FulltextIndexer(this);

    buildActions();

    // pages saved from the browser (OurNotes Connector)
    connector_ = new ConnectorServer(this);
    connector_->targetScope = [this] { return library_->scope(); };
    connect(connector_, &ConnectorServer::saved, this, [this](qint64, const QString&, const QString& title) {
        setStatus(tr("Saved from the browser: %1").arg(title), 8000);
    });
    connect(connector_, &ConnectorServer::metadataWanted, this,
            [this](qint64 lib, const QStringList& keys) { retrieveMetadata(lib, keys); });
    startConnector(AppContext::get().config().get("/connector/enabled", true).get<bool>());

    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) {
        if (i == 0)
            return;
        auto* w = tabs_->widget(i);
        // for Reopen Closed Tab, while this window is open
        if (auto* r = qobject_cast<ReaderTab*>(w))
            closedTabs_.push_back({r->libraryId(), r->attachmentKey(), i});
        else if (auto* p = dynamic_cast<PendingReaderTab*>(w))
            closedTabs_.push_back({p->libraryId, p->key, i});
        if (closedTabs_.size() > 50)
            closedTabs_.erase(closedTabs_.begin());
        tabs_->removeTab(i);
        w->deleteLater();
        releaseFreedMemory();  // a reader tab frees tens of megabytes
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        loadPendingTab(i);
        updateTabActions();
    });
    tabTitlesTimer_.setSingleShot(true);
    tabTitlesTimer_.setInterval(50);
    connect(&tabTitlesTimer_, &QTimer::timeout, this, &MainWindow::refreshTabTitles);
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString&, const QStringList&) {
                if (type == "item" || type == "*")
                    tabTitlesTimer_.start();
            });
    connect(library_, &LibraryView::openAttachmentRequested, this,
            [this](qint64 lib, const QString& key) { openAttachment(lib, key); });
    connect(library_, &LibraryView::exportRequested, this, &MainWindow::exportItems);
    connect(library_, &LibraryView::bibliographyRequested, this, &MainWindow::createBibliography);
    connect(library_, &LibraryView::retrieveMetadataRequested, this,
            [this](qint64 lib, const QStringList& keys) { retrieveMetadata(lib, keys); });
    connect(library_, &LibraryView::newLibraryRequested, this, &MainWindow::newLibrary);
    connect(library_, &LibraryView::deleteLibraryRequested, this, [this](qint64 id) { deleteLibrary(id); });
    connect(sync_, &SyncController::started, this, [this] {
        syncButton_->setIcon(icon("sync"));
        syncButton_->setEnabled(false);
        setStatus(tr("Syncing…"));
    });
    connect(sync_, &SyncController::progress, this, [this](const QString& m) { setStatus(m); });
    connect(sync_, &SyncController::finished, this, [this](bool ok, const QString& summary) {
        syncButton_->setEnabled(true);
        syncButton_->setIcon(icon(ok ? "sync" : "sync-error"));
        syncButton_->setToolTip(ok ? tr("Sync (last: %1)").arg(QTime::currentTime().toString("HH:mm")) : summary);
        setStatus(ok ? tr("Sync finished: %1").arg(summary) : tr("Sync failed: %1").arg(summary.section('\n', 0, 0)),
                  ok ? 8000 : 0);
    });
    connect(sync_, &SyncController::remoteLibraryDeleted, this, [this](qint64 id, bool ok, const QString& error) {
        auto& lib = AppContext::get().library();
        auto info = lib.library(id);
        if (!info)
            return;
        setStatus({});
        if (!ok)
            QMessageBox::warning(this, tr("Delete Library"),
                                 tr("\"%1\" could not be deleted on the sync target, so it was kept:\n\n%2")
                                     .arg(qs(info->name), error));
        else if (!lib.libraryContents(id).empty())  // filled in the meantime: the next sync uploads it again
            QMessageBox::information(this, tr("Delete Library"),
                                     tr("\"%1\" got new entries while it was being deleted, so it was kept.")
                                         .arg(qs(info->name)));
        else
            lib.deleteLibrary(id);
    });
    connect(sync_, &SyncController::fileDownloaded, this,
            [this](qint64 lib, const QString& key, bool ok, const QString& error) {
                if (ok)
                    openAttachment(lib, key);
                else
                    QMessageBox::warning(this, tr("Download File"), tr("Could not download the file: %1").arg(error));
            });
    connect(indexer_, &FulltextIndexer::progress, this, [this](int done, int total) {
        setStatus(tr("Indexing documents for full-text search: %1/%2").arg(done).arg(total), 3000);
    });

    auto& cfg = AppContext::get().config();
    auto geometry = QByteArray::fromBase64(QByteArray::fromStdString(cfg.get("/ui/geometry", "").get<std::string>()));
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    else
        resize(1440, 900);
    // Maximized last time, or too big for the screen (the first start at a large scale, or
    // a larger scale since): shown at a size that fits, then maximized. Shown maximized
    // straight away (KWin maximizes a window too big for the screen), it would be fitted to
    // the size steps Qt asks for above 100% before they are dropped (see dropX11SizeSteps),
    // and end a pixel or two short of the screen's edge.
    if (!isFullScreen() && QGuiApplication::platformName() == "xcb" && screen()->devicePixelRatio() != 1.0) {
        maximizeWhenShown_ = isMaximized();
        setWindowState(windowState() & ~Qt::WindowMaximized);
        auto room = screen()->availableGeometry().adjusted(0, 0, 0, -40);  // and a title bar
        if (width() > room.width() || height() > room.height()) {
            setGeometry(room.adjusted(room.width() / 20, room.height() / 20, -room.width() / 20, -room.height() / 20));
            maximizeWhenShown_ = true;
        }
    }
    auto state = QByteArray::fromBase64(QByteArray::fromStdString(cfg.get("/ui/libraryState", "").get<std::string>()));
    if (!state.isEmpty())
        library_->restoreState(state);
    indexer_->schedule(8000);
    if (sync_->hasTarget() && cfg.get("/sync/auto", true).get<bool>())
        QTimer::singleShot(3000, sync_, &SyncController::syncNow);
    updateTabActions();
    QTimer::singleShot(0, this, &MainWindow::restoreSession);  // once the window is shown
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::shutDown);
}

MainWindow::~MainWindow() = default;

void MainWindow::setStatus(const QString& text, int timeoutMs)
{
    status_->setText(text);
    if (timeoutMs > 0)
        QTimer::singleShot(timeoutMs, status_, [this, text] {
            if (status_->text() == text)
                status_->clear();
        });
}

void MainWindow::buildActions()
{
    // The item buttons sit on the library's search row; sync is with the tabs, as it
    // applies to everything.
    auto toolButton = [this](const char* iconName, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(icon(iconName));
        b->setIconSize(QSize(18, 18));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto withMenu = [](QToolButton* b, QMenu* menu) {
        b->setPopupMode(QToolButton::InstantPopup);
        b->setMenu(menu);
        return b;
    };

    // new item menu: common types first, all types in a submenu
    auto* newItemBtn = toolButton("new-item", tr("New Item"));
    auto* newItemMenu = new QMenu(newItemBtn);
    for (auto* t : {"journalArticle", "preprint", "conferencePaper", "book", "bookSection", "thesis", "report", "webpage"})
        newItemMenu->addAction(itemTypeIcon(t), qs(schema::typeLabel(t)), this, [this, t] { library_->newItem(t); });
    auto* more = newItemMenu->addMenu(tr("More"));
    for (auto& t : schema::itemTypes())
        more->addAction(itemTypeIcon(qs(t)), qs(schema::typeLabel(t)), this, [this, t] { library_->newItem(qs(t)); });
    library_->addTool(withMenu(newItemBtn, newItemMenu));

    auto* identifierBtn = toolButton("magic", tr("Add Item by Identifier (DOI, ISBN, arXiv, PMID)"));
    connect(identifierBtn, &QToolButton::clicked, this, &MainWindow::addByIdentifier);
    library_->addTool(identifierBtn);

    auto* noteBtn = toolButton("new-note", tr("New Note"));
    auto* noteMenu = new QMenu(noteBtn);
    noteMenu->addAction(tr("New Standalone Note"), this, [this] { library_->newNote(false); });
    noteMenu->addAction(tr("New Item Note"), this, [this] { library_->newNote(true); });
    library_->addTool(withMenu(noteBtn, noteMenu));

    auto* attachBtn = toolButton("attach", tr("Add Attachment"));
    auto* attachMenu = new QMenu(attachBtn);
    attachMenu->addAction(tr("Attach Stored Copy of File…"), this, [this] { library_->addAttachments(false); });
    attachMenu->addAction(tr("Attach Link to File…"), this, [this] { library_->addAttachments(true); });
    attachMenu->addAction(tr("Attach Link to URI…"), this, [this] { library_->addLinkAttachment(); });
    library_->addTool(withMenu(attachBtn, attachMenu));

    auto* collectionBtn = toolButton("new-collection", tr("New Collection"));
    connect(collectionBtn, &QToolButton::clicked, this, [this] { library_->newCollection(); });
    library_->addTool(collectionBtn);

    syncButton_ = toolButton("sync", tr("Sync"));
    syncButton_->setObjectName("syncButton");
    connect(syncButton_, &QToolButton::clicked, this, [this] {
        if (!sync_->hasTarget())
            showPreferences(1);
        else
            sync_->syncNow();
    });
    syncButton_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(syncButton_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(syncButton_);
        menu.addAction(tr("Sync History…"), this, &MainWindow::showSyncHistory);
        menu.addAction(tr("Storage and Sync Status…"), this, &MainWindow::showStorage);
        menu.addAction(tr("Sync Conflicts…"), this, &MainWindow::showConflicts);
        menu.addAction(tr("Sync Settings…"), this, [this] { showPreferences(1); });
        menu.exec(syncButton_->mapToGlobal(pos));
    });
    static_cast<TabWidget*>(tabs_)->addStripCornerWidget(syncButton_);

    // --- menus ----------------------------------------------------------------------------
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(icon("new-item"), tr("New Item"), this, [this] { library_->newItem("journalArticle"); });
    file->addAction(icon("new-note"), tr("New Note"), QKeySequence("Ctrl+Shift+O"), this,
                    [this] { library_->newNote(false); });
    file->addAction(icon("new-collection"), tr("New Collection…"), QKeySequence("Ctrl+Shift+N"), this,
                    [this] { library_->newCollection(); });
    file->addAction(tr("New Library…"), this, &MainWindow::newLibrary);
    file->addSeparator();
    file->addAction(icon("magic"), tr("Add by Identifier…"), QKeySequence("Ctrl+Shift+I"), this,
                    &MainWindow::addByIdentifier);
    file->addAction(icon("attach"), tr("Add Files…"), this, [this] { library_->addAttachments(false); });
    file->addSeparator();
    file->addAction(tr("Import…"), QKeySequence("Ctrl+Shift+M"), this, &MainWindow::importFile);
    file->addAction(tr("Import from Zotero…"), this, &MainWindow::importZotero);
    file->addAction(icon("export"), tr("Export Library…"), this,
                    [this] { exportItems({library_->libraryId(), ScopeKind::Library, {}}, {}); });
    auto* exportPdf = file->addAction(tr("Export PDF with Annotations…"), this, [this] {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->currentWidget()))
            r->exportPdf();
    });
    exportPdf->setObjectName("exportPdf");
    file->addSeparator();
    // Ctrl+W (⌘W) everywhere: Windows' standard Close is Ctrl+F4
    file->addAction(tr("Close Tab"), QKeySequence("Ctrl+W"), this, [this] {
        // macOS's menu bar serves all windows and takes ⌘W before the front one does: in
        // another window (Sync History, Storage and Sync Status, a dialog) it closes that
        // window, not a tab here (see installCloseShortcuts)
        if (auto* front = QApplication::activeWindow(); front && front != this) {
            front->close();
            return;
        }
        int i = tabs_->currentIndex();
        if (i > 0)
            emit tabs_->tabCloseRequested(i);
    });
    file->addAction(tr("Reopen Closed Tab"), QKeySequence("Ctrl+Shift+T"), this, &MainWindow::reopenClosedTab);
    file->addAction(tr("Quit"), QKeySequence::Quit, this, &QWidget::close)->setMenuRole(QAction::QuitRole);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("Copy Citation"), QKeySequence("Ctrl+Shift+A"), this, [this] { copyCitation(false); });
    edit->addAction(tr("Copy Bibliography"), QKeySequence("Ctrl+Shift+C"), this, [this] { copyCitation(true); });
    edit->addSeparator();
    edit->addAction(tr("Find"), QKeySequence::Find, this, [this] {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->currentWidget()))
            r->focusFind();
        else
            library_->focusSearch();
    });
    edit->addSeparator();
    auto* prefs = edit->addAction(tr("Preferences…"), QKeySequence::Preferences, this, [this] { showPreferences(0); });
    prefs->setMenuRole(QAction::PreferencesRole);

    auto* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Library Tab"), QKeySequence("Ctrl+1"), this, [this] { tabs_->setCurrentIndex(0); });
    auto* nextTab = view->addAction(tr("Next Tab"), QKeySequence::NextChild, this,
                                    [this] { tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count()); });
    auto* previousTab = view->addAction(tr("Previous Tab"), QKeySequence::PreviousChild, this, [this] {
        tabs_->setCurrentIndex((tabs_->currentIndex() + tabs_->count() - 1) % tabs_->count());
    });
    // Ctrl+Shift+Tab comes as Backtab or as Tab, by keyboard; missed, it went to the
    // nearest tab widget and switched Info and Notes instead
    previousTab->setShortcuts(QList<QKeySequence>{Qt::CTRL | Qt::SHIFT | Qt::Key_Tab} + previousTab->shortcuts());
    Q_UNUSED(nextTab);
#ifdef Q_OS_MACOS
    // Qt's Ctrl is the Command key on macOS, and Command-Tab switches applications; the
    // Control key (Qt's Meta) with Tab switches tabs, as in Safari and Finder
    nextTab->setShortcuts(QList<QKeySequence>{Qt::META | Qt::Key_Tab} + nextTab->shortcuts());
    previousTab->setShortcuts(QList<QKeySequence>{Qt::META | Qt::SHIFT | Qt::Key_Backtab, Qt::META | Qt::SHIFT | Qt::Key_Tab} +
                              previousTab->shortcuts());
#endif
    view->addSeparator();
    view->addAction(icon("zoom-in"), tr("Zoom In"), QKeySequence::ZoomIn, this, [this] {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->currentWidget()))
            r->view()->zoomIn();
    });
    view->addAction(icon("zoom-out"), tr("Zoom Out"), QKeySequence::ZoomOut, this, [this] {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->currentWidget()))
            r->view()->zoomOut();
    });
    view->addAction(icon("fit-width"), tr("Fit Width"), QKeySequence("Ctrl+0"), this, [this] {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->currentWidget()))
            r->view()->fitWidth();
    });
    view->addSeparator();
    // Control-Command-F on macOS, F11 elsewhere; a maximized window comes back maximized
    auto* fullScreen = view->addAction(tr("Enter Full Screen"), QKeySequence::FullScreen, this,
                                       [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
#ifdef Q_OS_MACOS
    fullScreen->setShortcut(QKeySequence(Qt::CTRL | Qt::META | Qt::Key_F));  // Command-Control-F, whatever the theme
#endif
    // Qt on macOS takes an item whose text starts with "Exit" for Quit and puts it in the
    // application menu: Quit toggled full screen, with Command-Control-F
    fullScreen->setMenuRole(QAction::NoRole);
    connect(view, &QMenu::aboutToShow, this, [this, fullScreen] {
        fullScreen->setText(isFullScreen() ? tr("Exit Full Screen") : tr("Enter Full Screen"));
    });

    auto* tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(icon("sync"), tr("Sync Now"), QKeySequence("Ctrl+Shift+S"), this, [this] {
        if (!sync_->hasTarget())
            showPreferences(1);
        else
            sync_->syncNow();
    });
    tools->addAction(tr("Sync History…"), this, &MainWindow::showSyncHistory);
    tools->addAction(tr("Sync Settings…"), this, [this] { showPreferences(1); });
    tools->addAction(tr("Sync Conflicts…"), this, &MainWindow::showConflicts);
    tools->addAction(tr("Storage and Sync Status…"), this, &MainWindow::showStorage);
    tools->addSeparator();
    tools->addAction(tr("Rebuild Full-Text Index"), this, [this] {
        indexer_->rebuild();
        setStatus(tr("Rebuilding the full-text index…"));
    });
    tools->addAction(icon("duplicates"), tr("Show Duplicate Items"), this, [this] {
        library_->collectionTree()->selectScope({library_->libraryId(), ScopeKind::Duplicates, {}});
    });

    auto* help = menuBar()->addMenu(tr("&Help"));
    auto* about = help->addAction(tr("About OurNotes"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::updateTabActions()
{
    bool reader = qobject_cast<ReaderTab*>(tabs_->currentWidget()) != nullptr;
    if (auto* a = findChild<QAction*>("exportPdf"))
        a->setEnabled(reader);
}

// --- attachments ---------------------------------------------------------------------------------

// The last tab closed, where it was; one already open again is only shown.
void MainWindow::reopenClosedTab()
{
    auto& lib = AppContext::get().library();
    while (!closedTabs_.empty()) {
        auto closed = closedTabs_.back();
        closedTabs_.pop_back();
        if (!lib.item(closed.libraryId, ss(closed.key)))
            continue;  // deleted meanwhile
        if (int open = findTab(closed.libraryId, closed.key); open >= 0) {
            tabs_->setCurrentIndex(open);
            return;
        }
        openAttachment(closed.libraryId, closed.key);
        int at = findTab(closed.libraryId, closed.key);
        if (at > 0 && closed.index < at)
            tabs_->tabBar()->moveTab(at, std::max(1, closed.index));
        return;
    }
}

void MainWindow::refreshTabTitles()
{
    bool changed = false;
    for (int i = 1; i < tabs_->count(); ++i) {
        QString title;
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->widget(i)))
            title = ReaderTab::titleFor(r->libraryId(), r->attachmentKey());
        else if (auto* p = dynamic_cast<PendingReaderTab*>(tabs_->widget(i)))
            title = ReaderTab::titleFor(p->libraryId, p->key);
        if (title.isEmpty() || title == tabs_->tabToolTip(i))  // (gone: its tab says so when shown)
            continue;
        tabs_->setTabText(i, tabText(title));
        tabs_->setTabToolTip(i, title);
        changed = true;
    }
    if (auto* tabs = qobject_cast<TabWidget*>(tabs_); tabs && changed)
        tabs->tabTextsChanged();
}

int MainWindow::findTab(int64_t libraryId, const QString& key) const
{
    for (int i = 1; i < tabs_->count(); ++i) {
        if (auto* r = qobject_cast<ReaderTab*>(tabs_->widget(i)))
            if (r->libraryId() == libraryId && r->attachmentKey() == key)
                return i;
        if (auto* p = dynamic_cast<PendingReaderTab*>(tabs_->widget(i)))
            if (p->libraryId == libraryId && p->key == key)
                return i;
    }
    return -1;
}

void MainWindow::loadPendingTab(int index)
{
    auto* pending = dynamic_cast<PendingReaderTab*>(tabs_->widget(index));
    if (!pending || !pending->label->text().isEmpty())
        return;  // not a restored tab, or one that could not be opened
    ReaderTab* reader = nullptr;
    try {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        reader = new ReaderTab(pending->libraryId, pending->key, tabs_);
        connect(reader, &ReaderTab::libraryRequested, this, [this] { tabs_->setCurrentIndex(0); });
        connect(reader, &ReaderTab::showInLibrary, this, [this](qint64, const QString& key) { selectItem(key); });
        QApplication::restoreOverrideCursor();
    } catch (const std::exception& e) {
        QApplication::restoreOverrideCursor();
        pending->label->setText(tr("Could not open the PDF:\n\n%1").arg(qs(e.what())));
        return;
    }
    QSignalBlocker block(tabs_);
    tabs_->insertTab(index, reader, tabs_->tabIcon(index), tabs_->tabText(index));
    tabs_->setTabToolTip(index, tabs_->tabToolTip(index + 1));
    tabs_->removeTab(index + 1);
    pending->deleteLater();
    tabs_->setCurrentIndex(index);
    reader->view()->setFocus();
}

void MainWindow::saveSession()
{
    Json tabs = Json::array();
    int selected = 0;
    for (int i = 1; i < tabs_->count(); ++i) {
        auto* w = tabs_->widget(i);
        Json tab;
        if (auto* r = qobject_cast<ReaderTab*>(w))
            tab = Json{{"libraryID", r->libraryId()}, {"key", ss(r->attachmentKey())}};
        else if (auto* p = dynamic_cast<PendingReaderTab*>(w))
            tab = Json{{"libraryID", p->libraryId}, {"key", ss(p->key)}};
        else
            continue;
        tabs.push_back(tab);
        if (i == tabs_->currentIndex())
            selected = (int)tabs.size();
    }
    AppContext::get().config().set(
        "/ui/session", Json{{"tabs", tabs}, {"selected", selected}, {"library", library_->viewState()}});
}

void MainWindow::restoreSession()
{
    auto session = AppContext::get().config().get("/ui/session", Json::object());
    if (!session.is_object())
        return;
    library_->restoreViewState(session.value("library", Json::object()));
    auto& lib = AppContext::get().library();
    for (auto& t : session.value("tabs", Json::array())) {
        int64_t libraryId = jint(t, "libraryID");
        auto key = jqs(t, "key");
        if (!lib.item(libraryId, ss(key)) || findTab(libraryId, key) >= 0)
            continue;  // deleted meanwhile, or already opened (e.g. from the command line)
        auto title = ReaderTab::titleFor(libraryId, key);
        int i = tabs_->addTab(new PendingReaderTab(libraryId, key), icon("type-pdf"), tabText(title));
        tabs_->setTabToolTip(i, title);
    }
    int selected = (int)jint(session, "selected");
    if (selected > 0 && selected < tabs_->count() && tabs_->currentIndex() == 0)
        tabs_->setCurrentIndex(selected);
}

void MainWindow::openAttachment(int64_t libraryId, const QString& key, int page, const QString& annotation)
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId, ss(key));
    if (!att)
        return;
    auto mode = jstr(*att, "linkMode");
    if (mode == "linked_url") {
        QDesktopServices::openUrl(QUrl(jqs(*att, "url")));
        return;
    }
    auto path = lib.attachmentFile(libraryId, *att);
    if (!lib.attachmentFileExists(libraryId, *att)) {
        if (schema::isStoredLinkMode(mode) && sync_->hasTarget()) {
            setStatus(tr("Downloading %1…").arg(jqs(*att, "filename")));
            sync_->downloadFile(libraryId, key);
            return;
        }
        QMessageBox::warning(this, tr("File Not Found"),
                             tr("The attached file could not be found at:\n\n%1").arg(qpath(path)));
        return;
    }
    bool isPdf = jstr(*att, "contentType") == "application/pdf" || toLower(pathString(path.extension())) == ".pdf";
    bool inApp = AppContext::get().config().get("/ui/openPdfIn", "tab").get<std::string>() == "tab";
    if (!isPdf || !inApp) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(path)));
        return;
    }
    ReaderTab* reader = nullptr;
    if (int existing = findTab(libraryId, key); existing >= 0) {
        tabs_->setCurrentIndex(existing);  // loads a restored tab
        reader = qobject_cast<ReaderTab*>(tabs_->widget(existing));
        if (!reader)
            return;  // could not be opened; the tab says why
    } else {
        try {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            reader = new ReaderTab(libraryId, key, tabs_);
            connect(reader, &ReaderTab::libraryRequested, this, [this] { tabs_->setCurrentIndex(0); });
            connect(reader, &ReaderTab::showInLibrary, this,
                    [this](qint64, const QString& key) { selectItem(key); });
            QApplication::restoreOverrideCursor();
        } catch (const std::exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Open PDF"), tr("Could not open the PDF:\n\n%1").arg(qs(e.what())));
            return;
        }
        auto title = reader->title();
        int i = tabs_->addTab(reader, icon("type-pdf"), tabText(title));
        tabs_->setTabToolTip(i, title);
    }
    tabs_->setCurrentWidget(reader);
    if (page >= 0)
        reader->goToPage(page);
    if (!annotation.isEmpty())
        reader->selectAnnotation(annotation);
    reader->view()->setFocus();
}

void MainWindow::selectItem(const QString& key)
{
    tabs_->setCurrentIndex(0);
    for (auto& l : AppContext::get().library().libraries())
        if (AppContext::get().library().item(l.id, ss(key))) {
            library_->selectItem(key, l.id);
            return;
        }
}

void MainWindow::openFiles(const QStringList& files)
{
    tabs_->setCurrentIndex(0);
    library_->importFiles(files, {});
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    // files from outside; the list's own items carry their files too, but dropped where
    // they do not fit they must not be imported again
    if (e->mimeData()->hasUrls() && !e->mimeData()->hasFormat(kItemsMime))
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    QStringList files;
    for (auto& url : e->mimeData()->urls())
        if (url.isLocalFile())
            files << url.toLocalFile();
    if (!files.isEmpty())
        openFiles(files);
}

// --- dialogs ---------------------------------------------------------------------------------------

void MainWindow::showPreferences(int page)
{
    PreferencesDialog dlg(sync_, this);
    dlg.showPage(page);
    connect(&dlg, &PreferencesDialog::importZoteroRequested, this, &MainWindow::importZotero);
    connect(&dlg, &PreferencesDialog::rebuildIndexRequested, indexer_, &FulltextIndexer::rebuild);
    connect(&dlg, &PreferencesDialog::conflictsRequested, this, &MainWindow::showConflicts);
    connect(&dlg, &PreferencesDialog::connectorToggled, this, &MainWindow::startConnector);
    dlg.exec();
    if (dlg.restartRequested())
        QTimer::singleShot(0, this, &MainWindow::restartApplication);
}

void MainWindow::startConnector(bool on)
{
    if (!on) {
        connector_->stop();
        return;
    }
    if (!connector_->isRunning() && !connector_->start())
        setStatus(tr("The browser connector cannot use port %1: another OurNotes is running?")
                      .arg(ConnectorServer::kDefaultPort),
                  10000);
}

void MainWindow::restartApplication()
{
    auto args = qApp->property("restartArguments").toStringList();
    if (close())
        QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, tr("About OurNotes"),
        tr("<h3>OurNotes %1</h3>"
           "<p>An open-source reference manager: collect and organise papers, read and annotate PDFs, write "
           "notes, and sync through your own server or a folder.</p>"
           "<p>Licensed under the GNU Affero General Public License v3.</p>"
           "<p>Built with Qt %2 and MuPDF. Item type schema from Zotero.</p>")
            .arg(OURNOTES_VERSION, qVersion()));
}

void MainWindow::newLibrary()
{
    bool ok = false;
    auto name = QInputDialog::getText(this, tr("New Library"), tr("Name:"), QLineEdit::Normal, tr("New Library"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    AppContext::get().library().createLibrary(ss(name.trimmed()), "group");
}

void MainWindow::deleteLibrary(int64_t libraryId)
{
    auto& lib = AppContext::get().library();
    auto info = lib.library(libraryId);
    if (!info)
        return;
    auto name = qs(info->name);
    auto refuse = [this](const QString& text) {
        QMessageBox box(QMessageBox::Information, tr("Delete Library"), text, QMessageBox::Ok, this);
        box.setObjectName("libraryNotDeletable");
        box.exec();
    };
    if (lib.libraries().size() <= 1) {
        refuse(tr("\"%1\" is the only library, and OurNotes needs one.").arg(name));
        return;
    }
    auto contents = lib.libraryContents(libraryId);
    if (!contents.empty()) {
        auto what = contents.items || contents.collections
                        ? tr("%1 and %2").arg(tr("%n item(s)", nullptr, (int)contents.items),
                                              tr("%n collection(s)", nullptr, (int)contents.collections))
                        : tr("%n entry(s) without a parent item", nullptr, (int)contents.other);
        refuse(tr("\"%1\" still has %2, counting its trash.\n\nOnly an empty library can be deleted: delete its "
                  "collections, move its items to the trash and empty the trash first.")
                   .arg(name, what));
        return;
    }
    auto target = AppContext::get().config().activeTarget();
    bool remote = target && info->syncTarget == target->id;
    auto question = remote ? tr("Delete the empty library \"%1\"?\n\nIt is also deleted on the sync target \"%2\", "
                                "and on your other devices when they next sync.")
                                 .arg(name, qs(target->name))
                           : tr("Delete the empty library \"%1\"?").arg(name);
    QMessageBox box(QMessageBox::Question, tr("Delete Library"), question, QMessageBox::NoButton, this);
    box.setObjectName("confirmDeleteLibrary");
    auto* yes = box.addButton(tr("Delete Library"), QMessageBox::DestructiveRole);
    box.setDefaultButton(box.addButton(QMessageBox::Cancel));
    box.exec();
    if (box.clickedButton() != static_cast<QAbstractButton*>(yes))
        return;
    if (!remote) {
        lib.deleteLibrary(libraryId);
        return;
    }
    if (sync_->isRunning()) {
        QMessageBox::information(this, tr("Delete Library"),
                                 tr("A sync is running. Delete the library when it has finished."));
        return;
    }
    setStatus(tr("Deleting \"%1\" on the sync target…").arg(name));
    sync_->deleteRemoteLibrary(libraryId);
}

void MainWindow::showStorage()
{
    if (!storage_) {
        storage_ = new StorageWindow(this);
        connect(storage_, &StorageWindow::syncSettingsRequested, this, [this] { showPreferences(1); });
    }
    storage_->refresh();
    storage_->show();
    storage_->raise();
    storage_->activateWindow();
}

void MainWindow::showSyncHistory()
{
    if (!syncHistory_) {
        syncHistory_ = new SyncHistoryWindow(this);
        syncHistory_->setObjectName("syncHistoryWindow");
        connect(sync_, &SyncController::historyChanged, syncHistory_,
                [this] { syncHistory_->reload(sync_->history()); });
        connect(syncHistory_, &SyncHistoryWindow::syncRequested, this, [this] {
            if (!sync_->hasTarget())
                showPreferences(1);
            else
                sync_->syncNow();
        });
        connect(syncHistory_, &SyncHistoryWindow::itemRequested, this,
                [this](qint64, const QString& key) { selectItem(key); });
        connect(syncHistory_, &SyncHistoryWindow::annotationRequested, this,
                [this](qint64 lib, const QString& attachment, const QString& annotation) {
                    openAttachment(lib, attachment, -1, annotation);
                });
    }
    syncHistory_->reload(sync_->history());
    syncHistory_->show();
    syncHistory_->raise();
    syncHistory_->activateWindow();
}

void MainWindow::showConflicts()
{
    auto conflicts = AppContext::get().library().conflicts();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Sync Conflicts"));
    dlg.resize(760, 520);
    auto* v = new QVBoxLayout(&dlg);
    auto* browser = new QTextBrowser(&dlg);
    QString html;
    if (conflicts.empty())
        html = "<p>" + tr("No conflicts were recorded.") + "</p>";
    for (auto& c : conflicts) {
        auto describe = [](const Json& d) {
            if (d.is_null())
                return QObject::tr("(deleted)");
            auto t = jqs(d, "title");
            if (t.isEmpty())
                t = qs(noteTitle(jstr(d, "note")));
            if (t.isEmpty())
                t = jqs(d, "name");
            if (t.isEmpty())
                t = jqs(d, "annotationText");
            return t.isEmpty() ? qs(d.dump()).left(200) : t;
        };
        html += QString("<p><b>%1 %2</b> — %3<br>%4: %5<br>%6: %7</p>")
                    .arg(qs(c.type), qs(c.key), qs(isoToLocalDisplay(c.created)),
                         c.kept == "local" ? tr("Kept (this computer)") : tr("Replaced (this computer)"),
                         describe(c.local).toHtmlEscaped(),
                         c.kept == "remote" ? tr("Kept (other device)") : tr("Replaced (other device)"),
                         describe(c.remote).toHtmlEscaped());
    }
    browser->setHtml(html);
    v->addWidget(browser);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto* clear = buttons->addButton(tr("Clear List"), QDialogButtonBox::ResetRole);
    connect(clear, &QPushButton::clicked, &dlg, [&] {
        for (auto& c : AppContext::get().library().conflicts())
            AppContext::get().library().deleteConflict(c.id);
        browser->setHtml("<p>" + tr("No conflicts were recorded.") + "</p>");
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    dlg.exec();
}

// --- Zotero import -------------------------------------------------------------------------------

void MainWindow::offerZoteroImport()
{
    auto dir = findZoteroDataDir();
    std::error_code ec;
    if (!fs::exists(dir / "zotero.sqlite", ec))
        return;
    auto answer = QMessageBox::question(
        this, tr("Import from Zotero"),
        tr("A Zotero library was found at %1.\n\nImport it into OurNotes now? Items, collections, notes, "
           "annotations and attachment files are copied; Zotero's own data is not modified.")
            .arg(qpath(dir)));
    if (answer == QMessageBox::Yes)
        importZotero();
}

void MainWindow::importZotero()
{
    auto guess = qpath(findZoteroDataDir());
    auto dir = QFileDialog::getExistingDirectory(this, tr("Zotero Data Directory"), guess);
    if (dir.isEmpty())
        return;
    if (!QFileInfo::exists(dir + "/zotero.sqlite")) {
        QMessageBox::warning(this, tr("Import from Zotero"), tr("No zotero.sqlite in %1.").arg(dir));
        return;
    }
    QProgressDialog progress(tr("Importing from Zotero…"), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    std::atomic<bool> cancel{false}, done{false};
    ZoteroImportReport report;
    QString error;
    auto dataDir = AppContext::get().library().dataDir();
    std::thread worker([&] {
        try {
            Library lib(dataDir);
            ZoteroImportOptions opt;
            opt.zoteroDir = fspath(dir);
            opt.cancelled = [&] { return cancel.load(); };
            opt.progress = [&](const std::string& stage, int n, int total) {
                QMetaObject::invokeMethod(
                    &progress,
                    [&progress, s = qs(stage), n, total] {
                        progress.setLabelText(s);
                        progress.setMaximum(total);
                        progress.setValue(n);
                    },
                    Qt::QueuedConnection);
            };
            report = on::importZotero(lib, opt);
        } catch (const std::exception& e) {
            error = qs(e.what());
        }
        done = true;
    });
    while (!done) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (progress.wasCanceled())
            cancel = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    worker.join();
    progress.close();
    AppContext::get().library().notifyRefresh();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Import from Zotero"), error);
        return;
    }
    QMessageBox::information(this, tr("Import from Zotero"), tr("Imported: %1").arg(qs(report.summary())));
    indexer_->schedule(2000);
}

// --- format-dependent actions ---------------------------------------------------------------------

void MainWindow::exportItems(const Scope& scope, const QStringList& keys) { exportItemsDialog(this, scope, keys); }

void MainWindow::importFile()
{
    auto s = library_->scope();
    importFileDialog(this, s.libraryId, s.kind == ScopeKind::Collection ? s.collectionKey : std::string());
}

void MainWindow::createBibliography(const Scope& scope, const QStringList& keys) { bibliographyDialog(this, scope, keys); }

void MainWindow::copyCitation(bool bibliography)
{
    copyCitationToClipboard(this, library_->libraryId(), library_->selectedTopLevelKeys(), bibliography);
}

void MainWindow::addByIdentifier()
{
    auto s = library_->scope();
    addByIdentifierDialog(this, s.libraryId, s.kind == ScopeKind::Collection ? s.collectionKey : std::string());
}

void MainWindow::retrieveMetadata(int64_t libraryId, const QStringList& keys)
{
    if (auto* dlg = on::gui::retrieveMetadata(this, libraryId, keys))
        connect(dlg, &MetadataProgressDialog::itemActivated, this,
                [this](qint64, const QString& key) { selectItem(key); });
}

void MainWindow::setTitleBarOverlap(int pixels)
{
    auto* layout = centralWidget()->layout();
    if (layout->contentsMargins().top() != pixels)
        layout->setContentsMargins(0, pixels, 0, 0);
}

// With the pointer at the top of the screen, macOS shows its menu and title bar over a
// full-screen window, where the tabs are; the window makes room for them. AppKit says
// nothing when they come and go, so the window looks: often while the pointer is near the
// top or they are shown, seldom otherwise.
void MainWindow::followTitleBar()
{
    int overlap = fullScreenTitleBarOverlap(this);
    setTitleBarOverlap(overlap);
    titleBarTimer_.setInterval(overlap > 0 || mapFromGlobal(QCursor::pos()).y() < 40 ? 16 : 200);
}

bool MainWindow::event(QEvent* e)
{
    bool handled = QMainWindow::event(e);
    if (e->type() == QEvent::LayoutRequest)
        followMinimumSize();
    if (e->type() == QEvent::Show && maximizeWhenShown_ && windowHandle())
        windowHandle()->installEventFilter(this);
    return handled;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* e)
{
    if (maximizeWhenShown_ && watched == windowHandle() && e->type() == QEvent::Expose && windowHandle()->isExposed()) {
        maximizeWhenShown_ = false;
        windowHandle()->removeEventFilter(this);
        // not while it is being exposed: KWin is still placing it, and fits it as above
        QTimer::singleShot(0, this, [this] {
            dropX11SizeSteps(this);
            setWindowState(windowState() | Qt::WindowMaximized);
        });
    }
    return QMainWindow::eventFilter(watched, e);
}

void MainWindow::followMinimumSize()
{
    if (isMaximized() || isFullScreen())
        return;  // (when restored)
    auto size = layout()->totalMinimumSize();
    if (size != minimumSize()) {
        setMinimumSize(size);
        dropX11SizeSteps(this);
    }
}

void MainWindow::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange)
        followMinimumSize();
#ifdef Q_OS_MACOS
    if (e->type() == QEvent::WindowStateChange) {
        if (isFullScreen()) {
            titleBarTimer_.start(200);
        } else {
            titleBarTimer_.stop();
            setTitleBarOverlap(0);
        }
    }
#endif
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    shutDown();
    e->accept();
}

// Saves the window, the open tabs and where each document was left, then closes the
// tabs. Runs once, whether the window is closed or the desktop session ends.
void MainWindow::shutDown()
{
    if (shutDown_)
        return;
    shutDown_ = true;
    auto& cfg = AppContext::get().config();
    saveSession();
    cfg.set("/ui/geometry", saveGeometry().toBase64().toStdString());
    cfg.set("/ui/libraryState", library_->saveState().toBase64().toStdString());
    try {
        cfg.save();
    } catch (const std::exception&) {
    }
    // close reader tabs first so their positions and notes are saved
    while (tabs_->count() > 1) {
        auto* w = tabs_->widget(1);
        tabs_->removeTab(1);
        delete w;
    }
    sync_->cancel();
}

} // namespace on::gui
