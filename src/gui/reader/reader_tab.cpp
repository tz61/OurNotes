#include "gui/reader/reader_tab.h"

#include <QActionGroup>
#include <QAction>
#include <QColorDialog>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QElapsedTimer>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QUndoStack>
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <utility>

#include "core/schema.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/item_pane.h"
#include "gui/note_editor.h"
#include "gui/search_field.h"
#include "gui/side_pane.h"
#include "gui/reader/annotation_sidebar.h"
#include "gui/theme.h"

namespace on::gui {

ReaderTab::ReaderTab(int64_t libraryId, const QString& attachmentKey, QWidget* parent)
    : QWidget(parent), libraryId_(libraryId), key_(attachmentKey)
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId, ss(attachmentKey));
    if (!att)
        throw Error("attachment not found");
    parentKey_ = jqs(*att, "parentItem");
    title_ = titleFor(libraryId, attachmentKey);
    editable_ = AppContext::get().editable(libraryId);

    view_ = new PdfView(this);
    view_->open(lib.attachmentFile(libraryId, *att));  // throws when the file is unreadable
    view_->setEditable(editable_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    toolbar_ = new QToolBar(this);
    toolbar_->setIconSize(QSize(18, 18));
    layout->addWidget(toolbar_);
    splitter_ = new PaneSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter_, 1);

    buildSidebar();
    splitter_->addWidget(view_);
    buildContextPane();
    buildToolbar();
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 0);
    splitter_->setSizes({260, 900, 320});
    splitter_->setCollapsible(1, false);

    connect(view_, &PdfView::annotationCreated, this, &ReaderTab::createAnnotation);
    connect(view_, &PdfView::annotationChanged, this, &ReaderTab::saveAnnotation);
    connect(view_, &PdfView::annotationDeleted, this, &ReaderTab::deleteAnnotation);
    connect(view_, &PdfView::annotationSelected, this, [this](const QString& key) {
        annotations_->setSelected(key, true);
        if (!key.isEmpty() && sidebar_->currentWidget() != annotations_)
            sidebar_->setCurrentWidget(annotations_);
    });
    connect(view_, &PdfView::toolChangeRequested, this, &ReaderTab::setTool);
    connect(view_, &PdfView::libraryRequested, this, &ReaderTab::libraryRequested);
    connect(view_, &PdfView::quoteToNote, this, [this](const PdfQuote& q) {
        addToNote("<blockquote><p>" + qs(referenceHtml(reference(q.page, q.place), ss(q.text))) + "</p></blockquote>");
    });
    connect(view_, &PdfView::referenceCopied, this,
            [this](const PdfQuote& q) { copyReference(reference(q.page, q.place), q.text); });
    connect(view_, &PdfView::annotationReferenceCopied, this, [this](const QString& key) {
        if (auto a = AppContext::get().library().item(libraryId_, ss(key)))
            copyReference(annotationReference(*a), jqs(*a, "annotationText"));
    });
    connect(view_, &PdfView::pageChanged, this, [this](int page) {
        updatePageLabel(page);
        positionTimer_.start();
        if (thumbnails_->count() > page) {
            QSignalBlocker b(thumbnails_);
            thumbnails_->setCurrentRow(page);
        }
    });
    connect(view_, &PdfView::zoomChanged, this,
            [this](double z) { zoomLabel_->setText(QString("%1%").arg(std::lround(z * 100))); });
    connect(view_, &PdfView::findStatus, this, [this](int cur, int total) {
        findStatus_->setText(total ? tr("%1 of %2").arg(cur).arg(total) : tr("No results"));
    });
    connect(view_, &PdfView::thumbnailRendered, this, [this](int page, const QImage& img) {
        if (auto* item = thumbnails_->item(page))
            item->setIcon(QPixmap::fromImage(img));
    });
    connect(view_, &PdfView::renderingRestarted, this, [this] {
        if (thumbnailsRequested_)
            requestThumbnails();
    });
    connect(view_, &PdfView::darkPagesChanged, this, [this] {
        updateDarkPagesAction();
        if (thumbnailsRequested_)
            requestThumbnails();
    });
    connect(annotations_, &AnnotationSidebar::activated, this, [this](const QString& key) {
        view_->selectAnnotation(key, true);
        annotations_->setSelected(key, false);
    });
    connect(annotations_, &AnnotationSidebar::changed, this, &ReaderTab::saveAnnotation);
    connect(annotations_, &AnnotationSidebar::deleted, this, &ReaderTab::deleteAnnotation);
    connect(annotations_, &AnnotationSidebar::addToNote, this, [this](const Json& a) {
        auto text = jstr(a, "annotationText");
        auto comment = jqs(a, "annotationComment");
        auto ref = annotationReference(a);
        QString html;
        if (!text.empty())
            html += "<p>" + qs(referenceHtml(ref, text)) + "</p>";
        if (!comment.isEmpty())
            html += "<p>" + comment.toHtmlEscaped() + (text.empty() ? " " + qs(referenceHtml(ref)) : QString()) + "</p>";
        addToNote(html);
    });
    connect(annotations_, &AnnotationSidebar::referenceCopied, this,
            [this](const Json& a) { copyReference(annotationReference(a), jqs(a, "annotationText")); });

    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(30);
    connect(&reloadTimer_, &QTimer::timeout, this, [this] {
        // dragged onto an item in the library, or off it
        if (auto att = AppContext::get().library().item(libraryId_, ss(key_)); att && jqs(*att, "parentItem") != parentKey_) {
            parentKey_ = jqs(*att, "parentItem");
            followParent();
        }
        reloadAnnotations();
        reloadNotes();
    });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64 lib, const QString& type, const QString&, const QStringList&) {
                if ((lib == libraryId_ || lib == 0) && (type == "item" || type == "*"))
                    reloadTimer_.start();
            });
    positionTimer_.setSingleShot(true);
    positionTimer_.setInterval(1500);
    connect(&positionTimer_, &QTimer::timeout, this, &ReaderTab::savePosition);

    reloadAnnotations();
    reloadNotes();
    // the paper's notes at hand: the top one is open next to the PDF
    if (!notesList_->currentItem() && notesList_->count() > 0)
        notesList_->setCurrentRow(0);
    // Where the document was left: the local state has the exact position; the synced
    // page wins when the document was read further on another computer (as in Zotero).
    auto state = lib.readerState(ss(key_));
    auto lastPage = lib.setting(libraryId, "lastPageIndex_" + ss(key_));
    if (lastPage.is_number_integer() && state.value("pageIndex", Json()) != lastPage)
        state = Json{{"pageIndex", lastPage}, {"scale", state.value("scale", Json())}};
    undo_ = new QUndoStack(this);
    undo_->setUndoLimit(50);
    auto* undoAct = new QAction(tr("Undo"), this);
    undoAct->setShortcut(QKeySequence::Undo);
    auto* redoAct = new QAction(tr("Redo"), this);
    auto redoKeys = QKeySequence::keyBindings(QKeySequence::Redo);
    if (!redoKeys.contains(QKeySequence(Qt::CTRL | Qt::Key_Y)))
        redoKeys.prepend(QKeySequence(Qt::CTRL | Qt::Key_Y));
    redoAct->setShortcuts(redoKeys);
    for (auto* a : {undoAct, redoAct}) {
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
    }
    connect(undoAct, &QAction::triggered, this, [this] {
        if (!forwardUndoToText(true))
            undo_->undo();
    });
    connect(redoAct, &QAction::triggered, this, [this] {
        if (!forwardUndoToText(false))
            undo_->redo();
    });
    stateTimer_.setSingleShot(true);
    stateTimer_.setInterval(1000);
    connect(&stateTimer_, &QTimer::timeout, this, &ReaderTab::saveViewState);
    QTimer::singleShot(0, this, [this, state] {
        view_->restoreViewState(state);
        updatePageLabel(view_->currentPage());
        // from now on, changes of the view are the user's
        connect(view_, &PdfView::viewChanged, &stateTimer_, qOverload<>(&QTimer::start));
        connect(view_, &PdfView::zoomChanged, &stateTimer_, qOverload<>(&QTimer::start));
        restored_ = true;
        for (auto& f : std::exchange(whenRestored_, {}))
            f();
    });
    QTimer::singleShot(400, this, &ReaderTab::importEmbeddedAnnotations);
}

ReaderTab::~ReaderTab()
{
    if (positionTimer_.isActive())
        savePosition();
    if (restored_)
        saveViewState();
    view_->finishComment();
    noteEditor_->flush();
}

// --- layout -------------------------------------------------------------------------------

void ReaderTab::buildSidebar()
{
    auto* side = new QWidget(splitter_);
    auto* v = new QVBoxLayout(side);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* switcher = new QHBoxLayout;
    switcher->setContentsMargins(4, 4, 4, 0);
    sidebar_ = new QStackedWidget(side);
    annotations_ = new AnnotationSidebar(sidebar_);
    annotations_->imageProvider = [this](const Json& a) { return view_->annotationImage(a, 1.5); };
    thumbnails_ = new QListWidget(sidebar_);
    thumbnails_->setViewMode(QListView::IconMode);
    thumbnails_->setFlow(QListView::LeftToRight);
    thumbnails_->setWrapping(true);
    thumbnails_->setResizeMode(QListView::Adjust);
    thumbnails_->setMovement(QListView::Static);
    thumbnails_->setIconSize(QSize(120, 160));
    thumbnails_->setSpacing(6);
    // Cells of one size, each with a blank page until its picture arrives: laid out
    // while they were only page numbers, the cells piled up as the pictures came,
    // until the list was resized.
    thumbnails_->setUniformItemSizes(true);
    thumbnails_->setGridSize(QSize(120 + 12, 160 + thumbnails_->fontMetrics().height() + 12));
    auto* doc = view_->document();
    auto first = doc->pageCount() > 0 ? doc->pageSize(0) : pdf::Size{};
    QSize blankSize = QSizeF(std::max(1.0f, first.width), std::max(1.0f, first.height))
                          .scaled(120, 160, Qt::KeepAspectRatio)
                          .toSize();
    QPixmap blank(blankSize);
    blank.fill(darkPdfPages() ? darkPageColor() : QColor(Qt::white));
    QIcon blankIcon(blank);
    for (int i = 0; i < doc->pageCount(); ++i)
        new QListWidgetItem(blankIcon, qs(doc->pageLabel(i)), thumbnails_);
    connect(thumbnails_, &QListWidget::currentRowChanged, this, [this](int row) { view_->goToPage(row); });
    outline_ = new QTreeWidget(sidebar_);
    outline_->setHeaderHidden(true);
    std::function<void(QTreeWidgetItem*, const std::vector<pdf::OutlineEntry>&)> fill =
        [&](QTreeWidgetItem* parent, const std::vector<pdf::OutlineEntry>& entries) {
            for (auto& e : entries) {
                auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(outline_);
                item->setText(0, qs(e.title));
                item->setData(0, Qt::UserRole, e.page);
                item->setData(0, Qt::UserRole + 1, (double)e.y);
                fill(item, e.children);
            }
        };
    fill(nullptr, doc->outline());
    outline_->expandToDepth(0);
    connect(outline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        int page = item->data(0, Qt::UserRole).toInt();
        if (page >= 0)
            view_->goToPage(page, item->data(0, Qt::UserRole + 1).toDouble());
    });
    sidebar_->addWidget(annotations_);
    sidebar_->addWidget(thumbnails_);
    sidebar_->addWidget(outline_);

    auto* group = new QActionGroup(side);
    auto addSwitch = [&](const QString& ic, const QString& tip, QWidget* page) {
        auto* b = new QToolButton(side);
        auto* a = new QAction(icon(ic), tip, group);
        a->setCheckable(true);
        b->setDefaultAction(a);
        b->setAutoRaise(true);
        connect(a, &QAction::triggered, this, [this, page] {
            sidebar_->setCurrentWidget(page);
            if (page == thumbnails_ && !thumbnailsRequested_) {
                thumbnailsRequested_ = true;
                requestThumbnails();
            }
        });
        switcher->addWidget(b);
        return a;
    };
    addSwitch("annotations", tr("Annotations"), annotations_)->setChecked(true);
    addSwitch("thumbnails", tr("Thumbnails"), thumbnails_);
    addSwitch("outline", tr("Outline"), outline_)->setEnabled(outline_->topLevelItemCount() > 0);
    switcher->addStretch();
    v->addLayout(switcher);
    v->addWidget(sidebar_, 1);
    splitter_->addWidget(side);
}

void ReaderTab::buildContextPane()
{
    contextPane_ = new QWidget(splitter_);
    auto* outer = new QVBoxLayout(contextPane_);
    outer->setContentsMargins(0, 0, 0, 0);
    contextTabs_ = new QTabWidget(contextPane_);
    contextTabs_->setObjectName("readerContextTabs");
    contextTabs_->setDocumentMode(true);
    outer->addWidget(contextTabs_);

    // Info: the paper's fields, tags and related items, here rather than in the library
    auto* infoPage = new QWidget;
    auto* iv = new QVBoxLayout(infoPage);
    iv->setContentsMargins(6, 6, 6, 6);
    info_ = new InfoEditor(infoPage);
    iv->addWidget(info_);
    tags_ = new TagsBox(infoPage);
    iv->addWidget(tags_);
    related_ = new RelatedBox(infoPage);
    iv->addWidget(related_);
    iv->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(infoPage);
    contextTabs_->addTab(scroll, tr("Info"));
    connect(related_, &RelatedBox::itemActivated, this,
            [this](const QString& key) { emit showInLibrary(libraryId_, key); });

    auto* notesPage = new QWidget;
    auto* v = new QVBoxLayout(notesPage);
    v->setContentsMargins(4, 4, 4, 4);
    auto* head = new QHBoxLayout;
    auto* add = new QToolButton(notesPage);
    add->setIcon(icon("new-note"));
    add->setToolTip(tr("Add note"));
    add->setPopupMode(QToolButton::InstantPopup);
    add->setAutoRaise(true);
    auto* addMenu = new QMenu(add);
    addMenu->addAction(tr("Add Item Note"), this, [this] { addToNote({}); });
    addMenu->addAction(tr("Add Item Note from Annotations"), this, &ReaderTab::addNoteFromAnnotations);
    add->setMenu(addMenu);
    add->setEnabled(editable_);
    add->setText(tr("Add Note"));
    add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->addStretch();
    head->addWidget(add);
    v->addLayout(head);
    auto* split = new QSplitter(Qt::Vertical, notesPage);
    notesList_ = new QListWidget(split);
    noteEditor_ = new NoteEditor(split);
    shareNotesSplit(split);
    v->addWidget(split, 1);
    connect(notesList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item)
            noteEditor_->setNote(libraryId_, item->data(Qt::UserRole).toString());
        else
            noteEditor_->clear();
    });
    enableNoteTrash(notesList_, [this] { return libraryId_; });
    contextTabs_->addTab(notesPage, icon("type-note"), tr("Notes"));
    shareInfoNotesTab(contextTabs_);  // Info or Notes, as in the other tabs and the library
    splitter_->addWidget(contextPane_);
    shareSidePaneWidth(splitter_);  // as wide as in the other tabs and the library
}

void ReaderTab::buildToolbar()
{
    auto* tb = toolbar_;
    sideToggle_ = tb->addAction(icon("sidebar"), tr("Show or hide the sidebar ([)"));
    sideToggle_->setObjectName("sideToggle");
    sideToggle_->setCheckable(true);
    sideToggle_->setChecked(true);
    connect(sideToggle_, &QAction::toggled, this, [this](bool on) { splitter_->widget(0)->setVisible(on); });
    tb->addSeparator();
    tb->addAction(icon("zoom-out"), tr("Zoom out (Ctrl+-)"), view_, &PdfView::zoomOut);
    zoomLabel_ = new QLabel("100%", tb);
    zoomLabel_->setMinimumWidth(44);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(zoomLabel_);
    tb->addAction(icon("zoom-in"), tr("Zoom in (Ctrl++)"), view_, &PdfView::zoomIn);
    tb->addAction(icon("fit-width"), tr("Fit width (F)"), view_, &PdfView::fitWidth);
    tb->addAction(icon("fit-page"), tr("Fit page (F twice)"), view_, &PdfView::fitPage);
    tb->addSeparator();
    tb->addAction(icon("prev"), tr("Previous page"), this, [this] { view_->goToPage(view_->currentPage() - 1); });
    pageEdit_ = new QLineEdit(tb);
    pageEdit_->setFixedWidth(48);
    pageEdit_->setAlignment(Qt::AlignCenter);
    tb->addWidget(pageEdit_);
    pageCount_ = new QLabel(QString(" / %1 ").arg(view_->pageCount()), tb);
    tb->addWidget(pageCount_);
    tb->addAction(icon("next"), tr("Next page"), this, [this] { view_->goToPage(view_->currentPage() + 1); });
    connect(pageEdit_, &QLineEdit::returnPressed, this, [this] {
        auto text = pageEdit_->text().trimmed();
        auto* doc = view_->document();
        for (int i = 0; i < doc->pageCount(); ++i)
            if (qs(doc->pageLabel(i)) == text) {
                view_->goToPage(i);
                return;
            }
        bool ok = false;
        int n = text.toInt(&ok);
        if (ok)
            view_->goToPage(n - 1);
    });
    tb->addSeparator();

    tools_ = new QActionGroup(this);
    struct ToolDef {
        ReaderTool tool;
        const char* icon;
        const char* tip;
    };
    const ToolDef defs[] = {{ReaderTool::Select, "tool-select", "Select text and annotations"},
                            {ReaderTool::Highlight, "tool-highlight", "Highlight text"},
                            {ReaderTool::Underline, "tool-underline", "Underline text"},
                            {ReaderTool::Note, "tool-note", "Add note"},
                            {ReaderTool::Text, "tool-text", "Add text"},
                            {ReaderTool::Area, "tool-area", "Select area"},
                            {ReaderTool::Ink, "tool-ink", "Draw"},
                            {ReaderTool::Eraser, "tool-eraser", "Eraser"}};
    int number = 0;
    for (auto& d : defs) {
        // 1 … 8 pick them while the page has the keyboard (see PdfView::keyPressEvent)
        auto* a = tb->addAction(icon(d.icon), tr("%1 (%2)").arg(tr(d.tip)).arg(++number));
        a->setCheckable(true);
        a->setData((int)d.tool);
        a->setActionGroup(tools_);
        a->setEnabled(editable_ || d.tool == ReaderTool::Select);
        if (d.tool == ReaderTool::Select)
            a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, tool = d.tool] { setTool(tool); });
    }
    colorButton_ = new QToolButton(tb);
    colorButton_->setObjectName("annotationColor");
    colorButton_->setPopupMode(QToolButton::InstantPopup);
    colorButton_->setToolTip(tr("Annotation colour (Q, W, E: your three)"));
    auto* colors = new QMenu(colorButton_);
    for (auto& c : schema::kAnnotationColors)
        colors->addAction(colorIcon(c.hex, 16), tr(c.name), this, [this, hex = QString(c.hex)] { useColor(hex); });
    colorButton_->setMenu(colors);
    auto color = qs(AppContext::get().config().get("/reader/color", "#ffd400").get<std::string>());
    view_->setColor(color);
    colorButton_->setIcon(colorIcon(color, 18));
    colorButton_->setEnabled(editable_);
    tb->addWidget(colorButton_);
    // Q, W and E: three colours of one's own, the same in every PDF, chosen here
    paletteButton_ = new QToolButton(tb);
    paletteButton_->setObjectName("colorPalette");
    paletteButton_->setPopupMode(QToolButton::InstantPopup);
    auto* palette = new QMenu(paletteButton_);
    const char* const keys[] = {"Q", "W", "E"};
    for (int i = 0; i < 3; ++i) {
        auto setColor = [i](const QString& hex) {
            auto colors = annotationPalette();
            colors[i] = hex;
            setAnnotationPalette(colors);
        };
        auto* menu = paletteMenus_[i] = palette->addMenu(QString());
        menu->setObjectName(QString("palette") + keys[i]);
        for (auto& c : schema::kAnnotationColors) {
            auto* a = menu->addAction(colorIcon(c.hex, 16), tr(c.name), this, [setColor, hex = QString(c.hex)] {
                setColor(hex);
            });
            a->setCheckable(true);
            a->setData(QString(c.hex));
        }
        menu->addSeparator();
        menu->addAction(tr("Other Colour…"), this, [this, i, key = QString(keys[i]), setColor] {
            auto c = QColorDialog::getColor(QColor(annotationPalette()[i]), this, tr("Colour of %1").arg(key));
            if (c.isValid())
                setColor(c.name());
        });
    }
    palette->addSeparator();
    palette->addAction(tr("Back to Yellow, Red and Green"), this,
                       [] { setAnnotationPalette({"#ffd400", "#ff6666", "#5fb236"}); });
    paletteButton_->setMenu(palette);
    tb->addWidget(paletteButton_);
    updatePalette();
    connect(&AppContext::get(), &AppContext::annotationPaletteChanged, this, &ReaderTab::updatePalette);
    connect(&AppContext::get(), &AppContext::appearanceChanged, this, &ReaderTab::updatePalette);  // the gear's ink
    connect(view_, &PdfView::paletteColorRequested, this,
            [this](int i) { useColor(annotationPalette().value(i)); });

    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    find_ = new SearchField(tb, "/reader/findHistory");
    find_->setObjectName("findInDocument");
    find_->setPlaceholderText(tr("Find in document"));
    find_->setClearButtonEnabled(true);
    find_->setFixedWidth(200);
    find_->addAction(icon("search"), QLineEdit::LeadingPosition);
    tb->addWidget(find_);
    tb->addAction(icon("up"), tr("Previous result"), this, [this] { view_->find(find_->text(), true); });
    tb->addAction(icon("down"), tr("Next result"), this, [this] { view_->find(find_->text(), false); });
    findStatus_ = new QLabel(tb);
    tb->addWidget(findStatus_);
    connect(find_, &QLineEdit::returnPressed, this, [this] { view_->find(find_->text(), false); });
    connect(find_, &SearchField::shiftReturnPressed, this, [this] { view_->find(find_->text(), true); });
    find_->addCaseToggle("/reader/findCaseSensitive");
    view_->setFindCaseSensitive(find_->caseSensitive());
    connect(find_, &SearchField::caseSensitivityChanged, this, [this](bool on) {
        view_->setFindCaseSensitive(on);
        if (!find_->text().trimmed().isEmpty())
            view_->find(find_->text(), false);  // the hits as they are now
    });
    // Esc: no word, no hits, the keys back on the page
    connect(find_, &SearchField::escapePressed, this, [this] {
        find_->clear();
        view_->clearFind();
        findStatus_->clear();
        view_->setFocus();
    });
    connect(find_, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty()) {
            view_->clearFind();
            findStatus_->clear();
        }
    });
    tb->addSeparator();
    // dark pages, or the PDF's own colours (e.g. for figures), in a dark or a light theme
    darkPages_ = tb->addAction(icon("dark-pages"), tr("Dark pages"));
    darkPages_->setObjectName("darkPages");
    darkPages_->setCheckable(true);
    connect(darkPages_, &QAction::triggered, this, [](bool on) { setDarkPdfPages(on); });
    // the page colour: white (or black) paper towards gray, for all PDFs
    pageTone_ = new QSlider(Qt::Horizontal, tb);
    pageTone_->setObjectName("pageTone");
    pageTone_->setRange(0, 100);
    pageTone_->setFixedWidth(80);
    pageTone_->setFocusPolicy(Qt::NoFocus);  // the page keeps the keys
    tb->addWidget(pageTone_);
    connect(pageTone_, &QSlider::valueChanged, this, [this](int tone) { setPageTone(view_->darkPages(), tone); });
    connect(pageTone_, &QSlider::sliderReleased, this, [] { AppContext::get().config().save(); });
    connect(&AppContext::get(), &AppContext::pageToneChanged, this, [this] {
        QSignalBlocker block(pageTone_);  // another PDF's slider moved
        pageTone_->setValue(pageTone(view_->darkPages()));
    });
    updateDarkPagesAction();
    paneToggle_ = tb->addAction(icon("context-pane"), tr("Show or hide Info and Notes (])"));
    paneToggle_->setObjectName("paneToggle");
    paneToggle_->setCheckable(true);
    paneToggle_->setChecked(true);
    connect(paneToggle_, &QAction::toggled, contextPane_, &QWidget::setVisible);
    // the keys on the page; without a parent item, there is no pane on the right
    connect(view_, &PdfView::panelToggleRequested, this, [this](bool right) {
        if (!right || !parentKey_.isEmpty())
            (right ? paneToggle_ : sideToggle_)->toggle();
    });
    connect(view_, &PdfView::contextTabRequested, this, [this](int tab) {
        if (parentKey_.isEmpty())
            return;
        paneToggle_->setChecked(true);
        chooseInfoNotesTab(contextTabs_, tab);
    });
    followParent();
    connect(view_, &PdfView::darkPagesToggleRequested, this, [this] { setDarkPdfPages(!view_->darkPages()); });
}

void ReaderTab::followParent()
{
    bool filed = !parentKey_.isEmpty();
    // "Add to Note": not in a library that cannot be changed (a group's, read only)
    bool notes = filed && AppContext::get().editable(libraryId_);
    view_->setNotesAvailable(notes);
    annotations_->setNotesAvailable(notes);
    paneToggle_->setVisible(filed);
    contextPane_->setVisible(filed && paneToggle_->isChecked());
    contextTabs_->setTabEnabled(0, filed);
    if (!filed) {
        noteEditor_->clear();
        return;
    }
    info_->setItem(libraryId_, parentKey_);
    tags_->setItems(libraryId_, {parentKey_});
    related_->setItem(libraryId_, parentKey_);
    showChosenInfoNotesTab(contextTabs_);
}

void ReaderTab::useColor(const QString& color)
{
    view_->setColor(color);
    colorButton_->setIcon(colorIcon(color, 18));
    AppContext::get().config().set("/reader/color", ss(color));
}

void ReaderTab::updatePalette()
{
    auto colors = annotationPalette();
    auto name = [](const QString& hex) {
        for (auto& c : schema::kAnnotationColors)
            if (QColor(c.hex) == QColor(hex))
                return tr(c.name);
        return QColor(hex).name();
    };
    paletteButton_->setIcon(paletteIcon(colors, 18));
    paletteButton_->setToolTip(tr("Q, W and E on a page: %1, %2 and %3, in every PDF. Click to change them.")
                                   .arg(name(colors[0]), name(colors[1]), name(colors[2])));
    const char* const keys[] = {"Q", "W", "E"};
    for (int i = 0; i < 3; ++i) {
        paletteMenus_[i]->setTitle(tr("%1: %2").arg(keys[i], name(colors[i])));
        paletteMenus_[i]->setIcon(colorIcon(colors[i], 16));
        for (auto* a : paletteMenus_[i]->actions())
            if (a->isCheckable())
                a->setChecked(QColor(a->data().toString()) == QColor(colors[i]));
    }
}

void ReaderTab::updateDarkPagesAction()
{
    if (!darkPages_)
        return;
    darkPages_->setChecked(view_->darkPages());
    darkPages_->setToolTip(view_->darkPages() ? tr("Showing pages in dark colours; click for the PDF's own colours (L)")
                                              : tr("Showing the PDF's own colours; click for dark pages (L)"));
    if (pageTone_) {
        QSignalBlocker block(pageTone_);
        pageTone_->setValue(pageTone(view_->darkPages()));  // the one remembered for these pages
        pageTone_->setToolTip(view_->darkPages() ? tr("Page colour: black to gray, for all PDFs")
                                                 : tr("Page colour: white to gray, for all PDFs"));
    }
}

void ReaderTab::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) {
        view_->setFocus();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ReaderTab::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        updateDarkPagesAction();
}

void ReaderTab::requestThumbnails()
{
    auto size = view_->document()->pageSize(0);
    view_->requestThumbnails(120.0 / std::max(1.0f, size.width) * devicePixelRatioF());
}

// --- behaviour --------------------------------------------------------------------------------

void ReaderTab::setTool(ReaderTool tool)
{
    view_->setTool(tool);
    for (auto* a : tools_->actions())
        if (a->data().toInt() == (int)tool)
            a->setChecked(true);
}

void ReaderTab::updatePageLabel(int page)
{
    if (!pageEdit_->hasFocus())
        pageEdit_->setText(qs(view_->document()->pageLabel(page)));
    pageCount_->setText(QString(" / %1 ").arg(view_->pageCount()));
}

void ReaderTab::goToPage(int page)
{
    if (!restored_)
        whenRestored_.push_back([this, page] { goToPage(page); });
    else
        view_->goToPage(page);
}

void ReaderTab::selectAnnotation(const QString& key)
{
    if (!restored_) {
        whenRestored_.push_back([this, key] { selectAnnotation(key); });
        return;
    }
    view_->selectAnnotation(key, true);
    annotations_->setSelected(key, true);
}

void ReaderTab::showPlace(int page, const QRectF& place)
{
    if (!restored_)
        whenRestored_.push_back([this, page, place] { showPlace(page, place); });
    else
        view_->showPlace(page, place);
}

void ReaderTab::focusFind()
{
    find_->setFocus();
    find_->selectAll();
}

QString ReaderTab::titleFor(int64_t libraryId, const QString& attachmentKey)
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId, ss(attachmentKey));
    if (!att)
        return {};
    auto title = jqs(*att, "title");
    auto parentKey = jstr(*att, "parentItem");
    if (!parentKey.empty())
        if (auto parentItem = lib.item(libraryId, parentKey)) {
            auto t = jqs(*parentItem, "title");
            if (!t.isEmpty())
                title = t;
        }
    return title;
}

void ReaderTab::saveViewState()
{
    stateTimer_.stop();
    AppContext::get().library().setReaderState(ss(key_), view_->viewState());
}

void ReaderTab::savePosition()
{
    AppContext::get().library().setSetting(libraryId_, "lastPageIndex_" + ss(key_), view_->currentPage());
}

void ReaderTab::reloadAnnotations()
{
    auto anns = AppContext::get().library().annotations(libraryId_, ss(key_));
    view_->setAnnotations(anns);
    annotations_->setAnnotations(anns, editable_);
}

void ReaderTab::reloadNotes()
{
    auto current = notesList_->currentItem() ? notesList_->currentItem()->data(Qt::UserRole).toString() : QString();
    QSignalBlocker block(notesList_);
    notesList_->clear();
    if (parentKey_.isEmpty())
        return;
    auto& lib = AppContext::get().library();
    for (auto& n : lib.children(libraryId_, ss(parentKey_), "note")) {
        if (jbool(n, "deleted"))
            continue;
        auto title = qs(noteTitle(jstr(n, "note")));
        auto* item = new QListWidgetItem(icon("type-note"), title.isEmpty() ? tr("(empty note)") : title, notesList_);
        item->setData(Qt::UserRole, jqs(n, "key"));
        if (jqs(n, "key") == current || jqs(n, "key") == noteEditor_->key())
            notesList_->setCurrentItem(item);
    }
    if (!notesList_->currentItem() && !noteEditor_->key().isEmpty())
        noteEditor_->clear();
}

void ReaderTab::importEmbeddedAnnotations()
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId_, ss(key_));
    if (!att || !editable_)
        return;
    auto marker = "embeddedAnnotations_" + ss(key_);
    auto md5 = jstr(*att, "md5");
    if (!md5.empty() && lib.setting(libraryId_, marker) == Json(md5))
        return;
    std::vector<Json> found;
    try {
        found = view_->document()->embeddedAnnotations();
    } catch (const std::exception&) {
        return;
    }
    // skip annotations already known (e.g. imported from Zotero)
    auto existing = lib.annotations(libraryId_, ss(key_));
    auto signature = [](const Json& a) {
        auto pos = a.value("annotationPosition", Json::object());
        std::string sig = jstr(a, "annotationType") + "|" + std::to_string(jint(pos, "pageIndex"));
        for (auto& r : pos.value("rects", Json::array()))
            for (auto& v : r)
                sig += "|" + std::to_string((long)std::lround(v.get<double>()));
        return sig;
    };
    std::set<std::string> known;
    for (auto& e : existing)
        if (jbool(e, "annotationIsExternal"))
            known.insert(signature(e));
    Library::BatchGuard batch(lib);
    for (auto& a : found) {
        if (known.count(signature(a)))
            continue;
        a["parentItem"] = ss(key_);
        lib.createItem(libraryId_, a);
    }
    if (!md5.empty())
        lib.setSetting(libraryId_, marker, md5);
}

// --- annotation edits and their history -------------------------------------------------------

// One change to an annotation: the annotation before and after (nullopt: not there).
// Undo puts back what was there before; the item keeps its key throughout.
class AnnotationEdit : public QUndoCommand {
public:
    AnnotationEdit(ReaderTab* tab, std::optional<Json> before, std::optional<Json> after, const QString& text)
        : QUndoCommand(text), tab_(tab), before_(std::move(before)), after_(std::move(after))
    {
        key_ = jstr(after_ ? *after_ : *before_, "key");
        time_.start();
    }

    void redo() override { apply(after_); }
    void undo() override { apply(before_); }
    int id() const override { return 1; }

    // Typing a comment changes the annotation with every key; it is one step.
    bool mergeWith(const QUndoCommand* other) override
    {
        auto* o = static_cast<const AnnotationEdit*>(other);
        if (o->key_ != key_ || !commentOnly() || !o->commentOnly() || time_.elapsed() > 3000)
            return false;
        after_ = o->after_;
        time_.start();
        return true;
    }

private:
    bool commentOnly() const
    {
        if (!before_ || !after_)
            return false;
        Json a = *before_, b = *after_;
        for (auto* field : {"annotationComment", "dateModified"}) {
            a.erase(field);
            b.erase(field);
        }
        return a == b;
    }

    void apply(const std::optional<Json>& state)
    {
        auto& lib = AppContext::get().library();
        bool exists = lib.item(tab_->libraryId_, key_).has_value();
        if (state && exists)
            lib.saveItem(tab_->libraryId_, *state);
        else if (state)
            lib.createItem(tab_->libraryId_, *state);
        else if (exists)
            lib.eraseItems(tab_->libraryId_, {key_});
        tab_->reloadAnnotations();
    }

    ReaderTab* tab_;
    std::optional<Json> before_, after_;
    std::string key_;
    QElapsedTimer time_;
};

void ReaderTab::pushAnnotationEdit(std::optional<Json> before, std::optional<Json> after, const QString& text)
{
    undo_->push(new AnnotationEdit(this, std::move(before), std::move(after), text));
}

// Ctrl+Y reaches the tab even when a text field has the focus (only Ctrl+Z is
// standard there); text fields keep their own history.
bool ReaderTab::forwardUndoToText(bool undo)
{
    auto* w = QApplication::focusWidget();
    if (auto* t = qobject_cast<QTextEdit*>(w))
        undo ? t->undo() : t->redo();
    else if (auto* p = qobject_cast<QPlainTextEdit*>(w))
        undo ? p->undo() : p->redo();
    else if (auto* l = qobject_cast<QLineEdit*>(w))
        undo ? l->undo() : l->redo();
    else
        return false;
    return true;
}

void ReaderTab::createAnnotation(const Json& data)
{
    Json d = data;
    d["parentItem"] = ss(key_);
    if (!isValidKey(jstr(d, "key")))
        d["key"] = newKey();
    pushAnnotationEdit(std::nullopt, d, tr("Add Annotation"));
}

void ReaderTab::saveAnnotation(const Json& data)
{
    if (jbool(data, "annotationIsExternal") || !editable_)
        return;
    auto before = AppContext::get().library().item(libraryId_, jstr(data, "key"));
    if (!before || *before == data)
        return;
    pushAnnotationEdit(*before, data, tr("Change Annotation"));
}

void ReaderTab::deleteAnnotation(const QString& key)
{
    auto& lib = AppContext::get().library();
    auto d = lib.item(libraryId_, ss(key));
    if (!d || jbool(*d, "annotationIsExternal") || !editable_)
        return;
    pushAnnotationEdit(*d, std::nullopt, tr("Delete Annotation"));
}

Reference ReaderTab::reference(int page, const QRectF& place) const
{
    std::optional<std::array<double, 4>> rect;
    if (!place.isNull())
        rect = std::array<double, 4>{place.left(), place.top(), place.right(), place.bottom()};
    auto label = view_->document() && page >= 0 ? view_->document()->pageLabel(page) : std::string();
    return referenceTo(AppContext::get().library(), libraryId_, ss(key_), page + 1, label, rect);
}

Reference ReaderTab::annotationReference(const Json& annotation) const
{
    if (AppContext::get().library().item(libraryId_, jstr(annotation, "key")))
        return referenceTo(AppContext::get().library(), libraryId_, jstr(annotation, "key"));
    auto position = annotation.value("annotationPosition", Json::object());
    int page = position.is_object() ? position.value("pageIndex", -1) : -1;
    return reference(page);
}

void ReaderTab::copyReference(const Reference& ref, const QString& quote)
{
    auto* mime = new QMimeData;
    mime->setHtml(qs(referenceHtml(ref, ss(quote))));
    mime->setText(qs(referenceText(ref, ss(quote))));
    QApplication::clipboard()->setMimeData(mime);
}

void ReaderTab::addToNote(const QString& html)
{
    if (!AppContext::get().editable(libraryId_))
        return;
    if (!noteEditor_->key().isEmpty()) {
        if (!html.isEmpty())
            noteEditor_->appendHtml(html);
        noteEditor_->focusEditor();
        return;
    }
    auto& lib = AppContext::get().library();
    QString body = html.isEmpty() ? QString() : "<div data-schema-version=\"9\">" + html + "</div>";
    auto note = lib.createNote(libraryId_, ss(body), ss(parentKey_));
    reloadNotes();
    for (int i = 0; i < notesList_->count(); ++i)
        if (notesList_->item(i)->data(Qt::UserRole).toString() == jqs(note, "key"))
            notesList_->setCurrentRow(i);
    noteEditor_->setNote(libraryId_, jqs(note, "key"));
    noteEditor_->focusEditor();
}

void ReaderTab::addNoteFromAnnotations()
{
    auto& lib = AppContext::get().library();
    auto anns = lib.annotations(libraryId_, ss(key_));
    QString html = "<h1>" + tr("Annotations") + "<br>(" + QLocale().toString(QDate::currentDate(), QLocale::ShortFormat) +
                   ")</h1>";
    for (auto& a : anns) {
        auto type = jqs(a, "annotationType");
        auto text = jstr(a, "annotationText");
        auto comment = jqs(a, "annotationComment");
        auto ref = annotationReference(a);
        if (type == "highlight" || type == "underline") {
            html += "<p>" + qs(referenceHtml(ref, text));
            if (!comment.isEmpty())
                html += " " + comment.toHtmlEscaped();
            html += "</p>";
        } else if (!comment.isEmpty()) {
            html += "<p>" + comment.toHtmlEscaped() + " " + qs(referenceHtml(ref)) + "</p>";
        }
    }
    auto note = lib.createNote(libraryId_, "<div data-schema-version=\"9\">" + ss(html) + "</div>", ss(parentKey_));
    reloadNotes();
    noteEditor_->setNote(libraryId_, jqs(note, "key"));
}

void ReaderTab::exportPdf()
{
    auto& lib = AppContext::get().library();
    auto att = lib.item(libraryId_, ss(key_));
    if (!att)
        return;
    auto suggested = qpath(lib.attachmentFile(libraryId_, *att).stem()) + " (annotated).pdf";
    auto file = QFileDialog::getSaveFileName(this, tr("Export PDF with Annotations"), suggested, "PDF (*.pdf)");
    if (file.isEmpty())
        return;
    try {
        view_->document()->exportWithAnnotations(fspath(file), lib.annotations(libraryId_, ss(key_)));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Export PDF"), qs(e.what()));
    }
}

} // namespace on::gui
