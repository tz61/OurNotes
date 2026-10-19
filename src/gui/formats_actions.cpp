#include "gui/formats_actions.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <atomic>
#include <thread>

#include "core/hash.h"
#include "core/schema.h"
#include "formats/citation.h"
#include "formats/formats.h"
#include "formats/metadata.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/items_model.h"
#include "gui/metadata_progress.h"

namespace fp::gui {

namespace {

// Run `work` on a worker thread while keeping the UI responsive; returns when done.
void runWithProgress(QWidget* parent, const QString& label, const std::function<void(std::atomic<bool>&)>& work)
{
    QProgressDialog progress(label, QObject::tr("Cancel"), 0, 0, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    std::atomic<bool> cancel{false}, done{false};
    std::thread worker([&] {
        work(cancel);
        done = true;
    });
    while (!done) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (progress.wasCanceled())
            cancel = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    worker.join();
}

std::vector<std::string> scopeKeys(const Scope& scope)
{
    auto& lib = AppContext::get().library();
    std::vector<std::string> out;
    std::set<std::string> allowed;
    bool filter = true;
    switch (scope.kind) {
    case ScopeKind::Collection:
        allowed = lib.collectionItemKeys(scope.libraryId, scope.collectionKey, true);
        break;
    case ScopeKind::Unfiled:
        allowed = lib.unfiledItemKeys(scope.libraryId);
        break;
    default:
        filter = false;
    }
    for (auto& r : lib.itemRows(scope.libraryId))
        if (r.parentKey.empty() && !r.trashed && (!filter || allowed.count(r.key)))
            out.push_back(r.key);
    return out;
}

// Regular items with their child notes and attachment paths, ready for an exporter.
std::vector<Json> exportableItems(int64_t libraryId, const std::vector<std::string>& keys, bool notes, bool files)
{
    auto& lib = AppContext::get().library();
    std::vector<Json> out;
    for (auto& d : lib.items(libraryId, keys)) {
        if (!schema::isRegularType(jstr(d, "itemType")))
            continue;
        Json item = d;
        Json noteList = Json::array(), attachments = Json::array();
        for (auto& c : lib.children(libraryId, jstr(d, "key"))) {
            if (jbool(c, "deleted"))
                continue;
            if (jstr(c, "itemType") == "note" && notes)
                noteList.push_back(jstr(c, "note"));
            if (jstr(c, "itemType") == "attachment" && files) {
                auto path = lib.attachmentFile(libraryId, c);
                attachments.push_back(Json{{"title", jstr(c, "title")},
                                           {"path", path.empty() ? "" : pathString(path)},
                                           {"contentType", jstr(c, "contentType")},
                                           {"url", jstr(c, "url")}});
            }
        }
        if (!noteList.empty())
            item["_notes"] = noteList;
        if (!attachments.empty())
            item["_attachments"] = attachments;
        out.push_back(std::move(item));
    }
    return out;
}

QString styleId() { return qs(AppContext::get().config().get("/ui/citationStyle", "apa").get<std::string>()); }

void setClipboard(const std::string& html, const std::string& text)
{
    auto* mime = new QMimeData;
    mime->setHtml(qs(html));
    mime->setText(qs(text));
    QApplication::clipboard()->setMimeData(mime);
}

// Create an item (and its child notes) from importer/lookup output.
Json createFromImport(int64_t libraryId, Json data, const std::string& collectionKey)
{
    auto& lib = AppContext::get().library();
    Json notes = data.value("_notes", Json::array());
    data.erase("_notes");
    data.erase("_files");
    data.erase("_attachments");
    if (!collectionKey.empty())
        data["collections"] = Json::array({collectionKey});
    auto item = lib.createItem(libraryId, data);
    for (auto& n : notes)
        if (n.is_string())
            lib.createNote(libraryId, n.get<std::string>(), jstr(item, "key"));
    return item;
}

} // namespace

// --- export / import ----------------------------------------------------------------------------

void exportItemsDialog(QWidget* parent, const Scope& scope, const QStringList& keys)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Export"));
    auto* form = new QFormLayout(&dlg);
    auto* format = new QComboBox(&dlg);
    for (auto& f : formats::allFormats())
        format->addItem(qs(f.label), qs(f.id));
    auto* notes = new QCheckBox(QObject::tr("Include notes"), &dlg);
    auto* files = new QCheckBox(QObject::tr("Include file paths"), &dlg);
    notes->setChecked(true);
    form->addRow(QObject::tr("Format:"), format);
    form->addRow(notes);
    form->addRow(files);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto* f = formats::findFormat(ss(format->currentData().toString()));
    if (!f || !f->exporter)
        return;
    auto itemKeys = keys.isEmpty() ? scopeKeys(scope) : toStdVector(keys);
    auto items = exportableItems(scope.libraryId, itemKeys, notes->isChecked(), files->isChecked());
    if (items.empty()) {
        QMessageBox::information(parent, QObject::tr("Export"), QObject::tr("There are no items to export."));
        return;
    }
    QString name = "Exported Items";
    if (scope.kind == ScopeKind::Collection)
        if (auto c = AppContext::get().library().collection(scope.libraryId, scope.collectionKey))
            name = jqs(*c, "name");
    auto file = QFileDialog::getSaveFileName(parent, QObject::tr("Export"), name + "." + qs(f->extension));
    if (file.isEmpty())
        return;
    try {
        writeFileAtomic(fspath(file), f->exporter(items));
    } catch (const std::exception& e) {
        QMessageBox::warning(parent, QObject::tr("Export"), qs(e.what()));
    }
}

void importFileDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey)
{
    auto file = QFileDialog::getOpenFileName(parent, QObject::tr("Import"), {},
                                             QObject::tr("Bibliographies (*.bib *.bibtex *.ris *.json);;All files (*)"));
    if (file.isEmpty())
        return;
    auto& lib = AppContext::get().library();
    std::vector<Json> items;
    try {
        auto text = readFile(fspath(file));
        auto id = formats::detectFormat(text);
        auto* f = formats::findFormat(id);
        if (!f || !f->importer)
            throw Error(ss(QObject::tr("The file format was not recognised (BibTeX, RIS and CSL JSON are supported).")));
        items = f->importer(text);
    } catch (const std::exception& e) {
        QMessageBox::warning(parent, QObject::tr("Import"), qs(e.what()));
        return;
    }
    if (items.empty()) {
        QMessageBox::information(parent, QObject::tr("Import"), QObject::tr("No items were found in the file."));
        return;
    }
    // like Zotero, put imported items into a new collection named after the file
    std::string target = collectionKey;
    if (target.empty() &&
        QMessageBox::question(parent, QObject::tr("Import"),
                              QObject::tr("Import %n item(s) into a new collection \"%1\"?", nullptr, (int)items.size())
                                  .arg(QFileInfo(file).completeBaseName()),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        target = jstr(lib.createCollection(libraryId, ss(QFileInfo(file).completeBaseName())), "key");
    auto base = fspath(QFileInfo(file).absolutePath());
    int attached = 0;
    Library::BatchGuard batch(lib);
    for (auto& d : items) {
        auto filesList = d.value("_files", Json::array());
        auto item = createFromImport(libraryId, d, target);
        for (auto& p : filesList) {
            if (!p.is_string())
                continue;
            auto path = u8path(p.get<std::string>());
            if (path.is_relative())
                path = base / path;
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                lib.importAttachment(libraryId, path, jstr(item, "key"));
                ++attached;
            }
        }
    }
    QMessageBox::information(parent, QObject::tr("Import"),
                             QObject::tr("Imported %1 item(s) and %2 file(s).").arg(items.size()).arg(attached));
}

// --- citations -------------------------------------------------------------------------------------

void copyCitationToClipboard(QWidget* parent, int64_t libraryId, const QStringList& keys, bool bibliography)
{
    auto items = exportableItems(libraryId, toStdVector(keys), false, false);
    if (items.empty())
        return;
    auto style = ss(styleId());
    auto fn = bibliography ? formats::formatBibliography : formats::formatCitation;
    setClipboard(fn(items, style, formats::OutputFormat::Html), fn(items, style, formats::OutputFormat::Text));
    Q_UNUSED(parent);
}

void bibliographyDialog(QWidget* parent, const Scope& scope, const QStringList& keys)
{
    auto items = exportableItems(scope.libraryId, keys.isEmpty() ? scopeKeys(scope) : toStdVector(keys), false, false);
    if (items.empty()) {
        QMessageBox::information(parent, QObject::tr("Create Bibliography"), QObject::tr("There are no items."));
        return;
    }
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Create Bibliography"));
    dlg.resize(720, 560);
    auto* v = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* style = new QComboBox(&dlg);
    for (auto& s : formats::builtinStyles())
        style->addItem(qs(s.title), qs(s.id));
    style->setCurrentIndex(std::max(0, style->findData(styleId())));
    auto* mode = new QComboBox(&dlg);
    mode->addItem(QObject::tr("Bibliography"));
    mode->addItem(QObject::tr("Citations"));
    form->addRow(QObject::tr("Citation style:"), style);
    form->addRow(QObject::tr("Output:"), mode);
    v->addLayout(form);
    auto* preview = new QTextBrowser(&dlg);
    v->addWidget(preview, 1);
    auto render = [&](formats::OutputFormat fmt) {
        auto id = ss(style->currentData().toString());
        return mode->currentIndex() == 0 ? formats::formatBibliography(items, id, fmt)
                                         : formats::formatCitation(items, id, fmt);
    };
    auto refresh = [&] { preview->setHtml(qs(render(formats::OutputFormat::Html))); };
    QObject::connect(style, &QComboBox::currentIndexChanged, &dlg, refresh);
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg, refresh);
    refresh();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto* copy = buttons->addButton(QObject::tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(QObject::tr("Save as HTML…"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dlg, [&] {
        setClipboard(render(formats::OutputFormat::Html), render(formats::OutputFormat::Text));
        AppContext::get().config().set("/ui/citationStyle", ss(style->currentData().toString()));
    });
    QObject::connect(save, &QPushButton::clicked, &dlg, [&] {
        auto file = QFileDialog::getSaveFileName(&dlg, QObject::tr("Save Bibliography"), "bibliography.html",
                                                 "HTML (*.html)");
        if (!file.isEmpty())
            writeFileAtomic(fspath(file), "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>" +
                                              render(formats::OutputFormat::Html) + "</body></html>");
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(buttons);
    dlg.exec();
    AppContext::get().config().set("/ui/citationStyle", ss(style->currentData().toString()));
}

// --- identifiers and metadata ----------------------------------------------------------------------

void addByIdentifierDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey)
{
    if (!libraryId || !AppContext::get().editable(libraryId))
        return;
    bool ok = false;
    auto text = QInputDialog::getMultiLineText(parent, QObject::tr("Add by Identifier"),
                                               QObject::tr("Enter DOIs, ISBNs, arXiv IDs or PMIDs (one per line):"),
                                               {}, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    auto ids = formats::parseIdentifiers(ss(text));
    if (ids.empty()) {
        QMessageBox::warning(parent, QObject::tr("Add by Identifier"), QObject::tr("No valid identifier was found."));
        return;
    }
    struct Result {
        formats::Identifier id;
        Json item;
        fs::path pdf;
        std::string error;
    };
    std::vector<Result> results;
    auto tmpDir = fs::temp_directory_path() / ("freepen-" + randomToken(6));
    runWithProgress(parent, QObject::tr("Looking up %n item(s)…", nullptr, (int)ids.size()), [&](std::atomic<bool>& cancel) {
        for (auto& id : ids) {
            if (cancel)
                break;
            Result r{id, nullptr, {}, {}};
            try {
                r.item = formats::lookup(id);
                if (id.kind == "arxiv" || jstr(r.item, "repository") == "arXiv") {
                    auto archive = jstr(r.item, "archiveID");
                    auto arxivId = startsWith(archive, "arXiv:") ? archive.substr(6) : id.value;
                    fs::create_directories(tmpDir);
                    auto dest = tmpDir / (safeFilename(arxivId) + ".pdf");
                    try {
                        formats::downloadFile(formats::arxivPdfUrl(arxivId), dest, true);
                        r.pdf = dest;
                    } catch (const std::exception&) {
                    }
                }
            } catch (const std::exception& e) {
                r.error = e.what();
            }
            results.push_back(std::move(r));
        }
    });
    auto& lib = AppContext::get().library();
    QStringList failed;
    int added = 0;
    for (auto& r : results) {
        if (!r.error.empty()) {
            failed << qs(r.id.value + ": " + r.error);
            continue;
        }
        auto item = createFromImport(libraryId, r.item, collectionKey);
        if (!r.pdf.empty())
            lib.importAttachment(libraryId, r.pdf, jstr(item, "key"));
        ++added;
    }
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    if (!failed.isEmpty())
        QMessageBox::warning(parent, QObject::tr("Add by Identifier"),
                             QObject::tr("Added %1 item(s). Not found:\n\n%2").arg(added).arg(failed.join("\n")));
}


QString applyMetadataMatch(int64_t libraryId, const QString& attachmentKey, const Json& item)
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId, ss(attachmentKey));
    if (!att || !jstr(*att, "parentItem").empty())
        return {};
    // the new parent takes over the attachment's collections
    auto collections = att->value("collections", Json::array());
    Library::BatchGuard batch(lib);
    auto parent = createFromImport(libraryId, item, {});
    if (!collections.empty())
        lib.updateItem(libraryId, jstr(parent, "key"), [&](Json& d) { d["collections"] = collections; });
    lib.updateItem(libraryId, ss(attachmentKey), [&](Json& d) {
        d["parentItem"] = jstr(parent, "key");
        d["title"] = "PDF";
        d.erase("collections");
    });
    return jqs(parent, "key");
}

MetadataProgressDialog* retrieveMetadata(QWidget* parent, int64_t libraryId, const QStringList& attachmentKeys)
{
    auto& lib = AppContext::get().library();
    std::vector<MetadataProgressDialog::Job> jobs;
    for (auto& k : attachmentKeys) {
        auto att = lib.item(libraryId, ss(k));
        if (!att || jstr(*att, "itemType") != "attachment" || !jstr(*att, "parentItem").empty() ||
            jstr(*att, "contentType") != "application/pdf")
            continue;
        auto path = lib.attachmentFile(libraryId, *att);
        std::error_code ec;
        if (!path.empty() && fs::is_regular_file(path, ec))
            jobs.push_back({k, qpath(path.filename()), path});
    }
    if (jobs.empty())
        return nullptr;
    auto* dlg = new MetadataProgressDialog(libraryId, std::move(jobs), parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->start();
    return dlg;
}

} // namespace fp::gui
