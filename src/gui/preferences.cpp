#include "gui/preferences.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSysInfo>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "core/hash.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/connector_server.h"
#include "gui/icons.h"
#include "gui/note_editor.h"
#include "gui/sync_controller.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"
#include "sync/http_transport.h"

namespace on::gui {

PreferencesDialog::PreferencesDialog(SyncController* sync, QWidget* parent) : QDialog(parent), sync_(sync)
{
    setWindowTitle(tr("OurNotes Preferences"));
    resize(620, 520);
    auto* v = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->addTab(generalPage(), tr("General"));
    tabs_->addTab(syncPage(), tr("Sync"));
    tabs_->addTab(advancedPage(), tr("Advanced"));
    v->addWidget(tabs_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    v->addWidget(buttons);
    connect(this, &QDialog::finished, this, &PreferencesDialog::save);
}

void PreferencesDialog::showPage(int index) { tabs_->setCurrentIndex(index); }

QWidget* PreferencesDialog::generalPage()
{
    auto& cfg = AppContext::get().config();
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    auto* dataDir = new QLabel(qpath(AppContext::get().library().dataDir()), page);
    dataDir->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(dataDir, 1);
    auto* change = new QPushButton(tr("Change…"), page);
    dirRow->addWidget(change);
    auto* open = new QPushButton(tr("Show"), page);
    dirRow->addWidget(open);
    form->addRow(tr("Data directory:"), dirRow);
    connect(open, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(AppContext::get().library().dataDir())));
    });
    connect(change, &QPushButton::clicked, this, [this, dataDir] {
        auto dir = QFileDialog::getExistingDirectory(this, tr("Choose Data Directory"), dataDir->text());
        if (dir.isEmpty())
            return;
        AppContext::get().config().setDataDir(fspath(dir));
        AppContext::get().config().save();
        dataDir->setText(dir);
        QMessageBox::information(this, tr("Data Directory"),
                                 tr("OurNotes will use the new data directory after a restart. Existing data is not "
                                    "moved; copy the folder yourself if you want to keep it."));
    });
    // interface scale (applied at startup, so changes need a restart)
    scale_ = new QComboBox(page);
    scale_->setObjectName("interfaceScale");
    int current = interfaceScale(cfg);
    for (int percent : {100, 110, 125, 150, 175, 200, 250, 300})
        scale_->addItem(QString("%1%").arg(percent), percent);
    if (scale_->findData(current) < 0)
        scale_->addItem(QString("%1%").arg(current), current);
    scale_->setCurrentIndex(scale_->findData(current));
    auto* scaleRow = new QHBoxLayout;
    scaleRow->addWidget(scale_);
    scaleHint_ = new QLabel(page);
    scaleHint_->setWordWrap(true);
    scaleRow->addWidget(scaleHint_, 1);
    auto* restart = new QPushButton(tr("Restart Now"), page);
    restart->setVisible(false);
    scaleRow->addWidget(restart);
    form->addRow(tr("Interface scale:"), scaleRow);
    if (interfaceScaleFromEnvironment()) {
        scale_->setEnabled(false);
        scaleHint_->setText(tr("Set by the QT_SCALE_FACTOR environment variable."));
    }
    connect(scale_, &QComboBox::currentIndexChanged, this, [this, restart, current] {
        bool changed = scale_->currentData().toInt() != current;
        scaleHint_->setText(changed ? tr("Takes effect after restarting OurNotes.") : QString());
        restart->setVisible(changed);
        AppContext::get().config().set("/ui/scale", scale_->currentData().toInt());
    });
    connect(restart, &QPushButton::clicked, this, [this] {
        restart_ = true;
        accept();
    });

    // light/dark theme, applied immediately
    appearance_ = new QComboBox(page);
    appearance_->setObjectName("appearance");
    appearance_->addItem(tr("Follow system"), appearanceKey(Appearance::System));
    appearance_->addItem(tr("Light"), appearanceKey(Appearance::Light));
    appearance_->addItem(tr("Dark"), appearanceKey(Appearance::Dark));
    appearance_->setCurrentIndex(appearance_->findData(appearanceKey(appearanceFromConfig(cfg))));
    form->addRow(tr("Appearance:"), appearance_);
    connect(appearance_, &QComboBox::currentIndexChanged, this, [this] {
        auto& config = AppContext::get().config();
        config.set("/ui/appearance", ss(appearance_->currentData().toString()));
        config.save();
        applyAppearance(appearanceFromConfig(config));
        emit AppContext::get().appearanceChanged();
    });
    auto* darkPages = new QCheckBox(tr("Show PDF pages in dark colours in dark mode"), page);
    darkPages->setObjectName("darkPages");
    darkPages->setChecked(cfg.get("/reader/darkPages", true).get<bool>());
    form->addRow(QString(), darkPages);
    connect(darkPages, &QCheckBox::toggled, this, [](bool on) {
        auto& ctx = AppContext::get();
        ctx.config().set("/reader/darkPages", on);
        ctx.config().save();
        emit ctx.appearanceChanged();
    });
    auto* bands = new QCheckBox(tr("Show a background picture behind the item list, a different one every two hours"),
                                page);
    bands->setObjectName("bandBackground");
    bands->setToolTip(tr("Pictures of your own in the data directory under backgrounds/bands are shown instead."));
    bands->setChecked(cfg.get("/ui/bandBackground", true).get<bool>());
    form->addRow(QString(), bands);
    connect(bands, &QCheckBox::toggled, this, [](bool on) {
        auto& ctx = AppContext::get();
        ctx.config().set("/ui/bandBackground", on);
        ctx.config().save();
        emit ctx.appearanceChanged();
    });
    form->addRow(tr("Formula colour:"), formulaColorRow(page));
    // the size notes start at; each note view also has its own −/+ (Ctrl+wheel)
    auto* noteZoom = new QComboBox(page);
    noteZoom->setObjectName("noteZoomDefault");
    int zoom = NoteEditor::defaultZoom();
    for (int percent : {80, 90, 100, 110, 125, 150, 175, 200})
        noteZoom->addItem(QString("%1%").arg(percent), percent);
    if (noteZoom->findData(zoom) < 0)
        noteZoom->addItem(QString("%1%").arg(zoom), zoom);
    noteZoom->setCurrentIndex(noteZoom->findData(zoom));
    noteZoom->setToolTip(tr("The text size of notes; each note view can be made larger or smaller with its "
                            "\u2212 and + buttons or Ctrl+wheel."));
    form->addRow(tr("Note text size:"), noteZoom);
    connect(noteZoom, &QComboBox::currentIndexChanged, this, [noteZoom] {
        auto& ctx = AppContext::get();
        ctx.config().set("/notes/zoom", noteZoom->currentData().toInt());
        ctx.config().save();
        emit ctx.appearanceChanged();
    });

    openPdf_ = new QComboBox(page);
    openPdf_->addItem(tr("in OurNotes"), "tab");
    openPdf_->addItem(tr("with the system PDF viewer"), "external");
    openPdf_->setCurrentIndex(openPdf_->findData(qs(cfg.get("/ui/openPdfIn", "tab").get<std::string>())));
    form->addRow(tr("Open PDFs:"), openPdf_);
    subcollections_ = new QCheckBox(tr("Show items from subcollections"), page);
    subcollections_->setChecked(cfg.get("/ui/showSubcollectionItems", false).get<bool>());
    form->addRow(QString(), subcollections_);
    fulltext_ = new QCheckBox(tr("Index the text of PDFs for full-text search"), page);
    fulltext_->setChecked(cfg.get("/fulltext/enabled", true).get<bool>());
    form->addRow(QString(), fulltext_);
    return page;
}

// Formulas in notes use the text colour (white in dark mode) unless a colour is chosen.
QWidget* PreferencesDialog::formulaColorRow(QWidget* page)
{
    auto* row = new QWidget(page);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* mode = new QComboBox(row);
    mode->setObjectName("formulaColor");
    mode->addItem(tr("Same as text"), false);
    mode->addItem(tr("Custom"), true);
    auto* swatch = new QPushButton(row);
    swatch->setObjectName("formulaColorSwatch");
    swatch->setToolTip(tr("Choose the formula colour"));
    h->addWidget(mode);
    h->addWidget(swatch);
    auto* hint = new QLabel(tr("Single formulas can use \\textcolor{red}{…}."), row);
    hint->setObjectName("formulaColorHint");
    hint->setForegroundRole(QPalette::PlaceholderText);
    // one line: wrapped, macOS's form kept it as narrow as its size hint, on two lines
    // without room for the second
    h->addWidget(hint, 1);

    auto stored = [] { return qs(AppContext::get().config().get("/notes/formulaColor", "").get<std::string>()); };
    auto refresh = [mode, swatch, stored] {
        QColor c(stored());
        bool custom = c.isValid();
        QSignalBlocker block(mode);
        mode->setCurrentIndex(custom ? 1 : 0);
        swatch->setVisible(custom);
        int s = swatch->fontMetrics().height();
        swatch->setIcon(colorIcon(custom ? c.name() : QString(), s));
        swatch->setText(custom ? c.name() : QString());
    };
    auto store = [](const QString& value) {
        auto& config = AppContext::get().config();
        config.set("/notes/formulaColor", ss(value));
        config.save();
        emit AppContext::get().appearanceChanged();
    };
    auto choose = [this, stored, store, refresh] {
        QColor start(stored());
        auto c = QColorDialog::getColor(start.isValid() ? start : palette().color(QPalette::Text), this,
                                        tr("Formula Colour"));
        if (c.isValid())
            store(c.name());
        refresh();
    };
    connect(mode, &QComboBox::currentIndexChanged, this, [mode, store, refresh, choose] {
        if (mode->currentData().toBool())
            choose();
        else {
            store(QString());
            refresh();
        }
    });
    connect(swatch, &QPushButton::clicked, this, choose);
    refresh();
    return row;
}

QWidget* PreferencesDialog::syncPage()
{
    auto& cfg = AppContext::get().config();
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    auto* intro = new QLabel(tr("Sync your libraries with a <b>ournotesd</b> server (run it on any machine you "
                                "control) or with a <b>folder</b> such as a USB drive or a network share. "
                                "The active target is used by the Sync button."),
                             page);
    intro->setWordWrap(true);
    v->addWidget(intro);
    targets_ = new QListWidget(page);
    v->addWidget(targets_, 1);
    auto* row = new QHBoxLayout;
    auto* addServer = new QPushButton(icon("sync"), tr("Add Server…"), page);
    auto* addFolder = new QPushButton(icon("folder"), tr("Add Folder…"), page);
    auto* makeActive = new QPushButton(tr("Use for Sync"), page);
    auto* remove = new QPushButton(tr("Remove"), page);
    row->addWidget(addServer);
    row->addWidget(addFolder);
    row->addStretch();
    row->addWidget(makeActive);
    row->addWidget(remove);
    v->addLayout(row);
    connect(addServer, &QPushButton::clicked, this, &PreferencesDialog::addServer);
    connect(addFolder, &QPushButton::clicked, this, &PreferencesDialog::addFolder);
    connect(makeActive, &QPushButton::clicked, this, [this] {
        if (auto* item = targets_->currentItem()) {
            AppContext::get().config().setActiveTarget(ss(item->data(Qt::UserRole).toString()));
            AppContext::get().config().save();
            reloadTargets();
            sync_->reloadSettings();
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        auto* item = targets_->currentItem();
        if (!item)
            return;
        if (QMessageBox::question(this, tr("Remove Sync Target"),
                                  tr("Remove \"%1\"? Nothing is deleted on the server or in the folder.")
                                      .arg(item->text())) != QMessageBox::Yes)
            return;
        auto id = ss(item->data(Qt::UserRole).toString());
        if (auto t = AppContext::get().config().target(id); t && t->kind == "remote" && !t->token.empty()) {
            try {
                sync::HttpTransport(t->url, t->token, t->verifyTls).logout();
            } catch (const std::exception&) {
            }
        }
        AppContext::get().config().removeTarget(id);
        AppContext::get().config().save();
        reloadTargets();
        sync_->reloadSettings();
    });

    auto* options = new QGroupBox(tr("Options"), page);
    auto* form = new QFormLayout(options);
    autoSync_ = new QCheckBox(tr("Sync automatically after changes, here or on another computer"), options);
    autoSync_->setToolTip(tr("Another computer's changes arrive within seconds from a server (OurNotes 0.1.10 or "
                             "later), within a few seconds from a folder."));
    autoSync_->setChecked(cfg.get("/sync/auto", true).get<bool>());
    form->addRow(autoSync_);
    interval_ = new QSpinBox(options);
    interval_->setRange(0, 1440);
    interval_->setSuffix(tr(" min"));
    interval_->setSpecialValueText(tr("never"));
    interval_->setValue(cfg.get("/sync/intervalMinutes", 15).get<int>());
    form->addRow(tr("Also sync every:"), interval_);
    fileMode_ = new QComboBox(options);
    fileMode_->addItem(tr("Download attachment files during sync"), "on_sync");
    fileMode_->addItem(tr("Download attachment files when opened"), "on_demand");
    fileMode_->setCurrentIndex(fileMode_->findData(qs(cfg.get("/sync/files", "on_sync").get<std::string>())));
    form->addRow(tr("Files:"), fileMode_);
    v->addWidget(options);

    auto* syncRow = new QHBoxLayout;
    syncStatus_ = new QLabel(page);
    syncStatus_->setWordWrap(true);
    auto* syncNow = new QPushButton(icon("sync"), tr("Sync Now"), page);
    syncRow->addWidget(syncStatus_, 1);
    syncRow->addWidget(syncNow);
    v->addLayout(syncRow);
    connect(syncNow, &QPushButton::clicked, this, [this] {
        save();
        sync_->syncNow();
    });
    connect(sync_, &SyncController::progress, syncStatus_, &QLabel::setText);
    connect(sync_, &SyncController::finished, this,
            [this](bool ok, const QString& summary) { syncStatus_->setText((ok ? tr("Done: ") : tr("Failed: ")) + summary); });
    reloadTargets();
    return page;
}

QWidget* PreferencesDialog::advancedPage()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    auto add = [&](const QString& text, const QString& help, auto slot) {
        auto* b = new QPushButton(text, page);
        connect(b, &QPushButton::clicked, this, slot);
        v->addWidget(b);
        auto* l = new QLabel(help, page);
        l->setWordWrap(true);
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        l->setFont(f);
        v->addWidget(l);
        v->addSpacing(8);
    };
    add(tr("Import from Zotero…"), tr("Copies libraries, collections, items, notes, annotations and files from a "
                                       "Zotero data directory. Running it again updates items changed in Zotero."),
        [this] { emit importZoteroRequested(); });
    add(tr("Rebuild Full-Text Index"), tr("Re-extracts the text of all PDFs used by the \"Everything\" search mode."),
        [this] { emit rebuildIndexRequested(); });
    add(tr("Sync Conflicts…"), tr("Shows edits that were replaced during sync by a newer change made elsewhere."),
        [this] { emit conflictsRequested(); });
    add(tr("Open Configuration Folder"), qpath(AppContext::get().config().path()), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(qpath(AppContext::get().config().path().parent_path())));
    });
    auto* connector = new QCheckBox(tr("Let the OurNotes Connector browser extension save pages"), page);
    connector->setObjectName("connectorEnabled");
    connector->setChecked(AppContext::get().config().get("/connector/enabled", true).get<bool>());
    v->addWidget(connector);
    auto* connectorHelp =
        new QLabel(tr("The extension (in the browser folder of OurNotes' source) sends the page you are reading to "
                      "OurNotes at 127.0.0.1:%1, into the collection selected in the library. Only browser "
                      "extensions can use it, and only on this computer.")
                       .arg(ConnectorServer::kDefaultPort),
                   page);
    connectorHelp->setWordWrap(true);
    QFont small = connectorHelp->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    connectorHelp->setFont(small);
    v->addWidget(connectorHelp);
    connect(connector, &QCheckBox::toggled, this, [this](bool on) {
        auto& cfg = AppContext::get().config();
        cfg.set("/connector/enabled", on);
        cfg.save();
        emit connectorToggled(on);
    });
    v->addStretch();
    return page;
}

void PreferencesDialog::reloadTargets()
{
    targets_->clear();
    auto& cfg = AppContext::get().config();
    auto active = cfg.activeTarget();
    for (auto& t : cfg.targets()) {
        bool isActive = active && active->id == t.id;
        auto* item = new QListWidgetItem(icon(t.kind == "local" ? "folder" : "sync"),
                                         qs(t.describe()) + (isActive ? "   " + tr("(active)") : QString()), targets_);
        item->setData(Qt::UserRole, qs(t.id));
        if (isActive) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }
}

void PreferencesDialog::addServer()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Add ournotesd Server"));
    auto* form = new QFormLayout(&dlg);
    auto* name = new QLineEdit(&dlg);
    auto* url = new QLineEdit(&dlg);
    url->setPlaceholderText("https://example.org:8642");
    auto* user = new QLineEdit(&dlg);
    auto* pass = new QLineEdit(&dlg);
    pass->setEchoMode(QLineEdit::Password);
    auto* verify = new QCheckBox(tr("Verify TLS certificate"), &dlg);
    verify->setChecked(true);
    auto* help = new QLabel(tr("Create an account on the server with <code>ournotesd useradd NAME</code>. "
                               "Only a sign-in token is stored, not the password."),
                            &dlg);
    help->setWordWrap(true);
    form->addRow(tr("Name:"), name);
    form->addRow(tr("Server URL:"), url);
    form->addRow(tr("Username:"), user);
    form->addRow(tr("Password:"), pass);
    form->addRow(QString(), verify);
    form->addRow(help);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Sign In"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        auto u = url->text().trimmed();
        if (!u.contains("://"))
            u = "https://" + u;
        try {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            auto device = QSysInfo::machineHostName();
            auto token = sync::HttpTransport::login(ss(u), ss(user->text().trimmed()), ss(pass->text()),
                                                    ss("OurNotes on " + device), verify->isChecked());
            QApplication::restoreOverrideCursor();
            SyncTarget t;
            t.id = randomToken(4);
            t.kind = "remote";
            t.name = ss(name->text().trimmed().isEmpty() ? QUrl(u).host() : name->text().trimmed());
            t.url = ss(u);
            t.username = ss(user->text().trimmed());
            t.token = token;
            t.verifyTls = verify->isChecked();
            auto& cfg = AppContext::get().config();
            cfg.upsertTarget(t);
            if (!cfg.get("/sync/active").is_string())
                cfg.setActiveTarget(t.id);
            cfg.save();
            dlg.accept();
        } catch (const std::exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(&dlg, tr("Sign In"), tr("Could not sign in: %1").arg(qs(e.what())));
        }
    });
    if (dlg.exec() == QDialog::Accepted) {
        reloadTargets();
        sync_->reloadSettings();
    }
}

void PreferencesDialog::addFolder()
{
    auto dir = QFileDialog::getExistingDirectory(this, tr("Choose a Sync Folder"));
    if (dir.isEmpty())
        return;
    SyncTarget t;
    t.id = randomToken(4);
    t.kind = "local";
    t.name = ss(QFileInfo(dir).fileName().isEmpty() ? dir : QFileInfo(dir).fileName());
    t.path = ss(dir);
    auto& cfg = AppContext::get().config();
    cfg.upsertTarget(t);
    if (!cfg.get("/sync/active").is_string())
        cfg.setActiveTarget(t.id);
    cfg.save();
    reloadTargets();
    sync_->reloadSettings();
}

void PreferencesDialog::save()
{
    auto& cfg = AppContext::get().config();
    cfg.set("/ui/scale", scale_->currentData().toInt());
    cfg.set("/ui/openPdfIn", ss(openPdf_->currentData().toString()));
    cfg.set("/ui/showSubcollectionItems", subcollections_->isChecked());
    cfg.set("/fulltext/enabled", fulltext_->isChecked());
    cfg.set("/sync/auto", autoSync_->isChecked());
    cfg.set("/sync/intervalMinutes", interval_->value());
    cfg.set("/sync/files", ss(fileMode_->currentData().toString()));
    cfg.save();
    sync_->reloadSettings();
}

} // namespace on::gui
