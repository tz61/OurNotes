# Audit: memory, performance, rendering (October 2026)

An audit of the code at 0.2.1 for memory leaks, performance problems and rendering
errors, and then for anything else suspicious. Each finding is either **fixed** (the commit
is on this branch) or **reported** (with why it was left).

How it was checked: Qt 6.8.4 and MuPDF 1.28.5 (conda-forge), the full build, and
`ctest` with the GUI tests offscreen, before and after each round of fixes. All 7 test
programs pass before and after.

> Note for anyone repeating this: with conda-forge's FreeType 2.14, `test_gui` crashed
> in `QFontEngineFT::loadGlyph` ("render glyph failed err=62") while MicroTeX drew a
> formula. That is the toolchain, not OurNotes: with FreeType 2.12 preloaded every test
> passes. CI (Ubuntu's FreeType) is not affected.

## Round 1: fixed

### PDF reader (`src/gui/reader`, `src/pdf`)

| Kind | Where | What |
|---|---|---|
| perf | `render_worker.cpp` `setQueue`/`run` | The page being rendered was queued again on every scroll step and rendered twice. The request in hand is now dropped from new queues. |
| perf | `render_worker.cpp` `addRequests` | Thumbnails asked for again (dark pages toggled) piled up behind the old ones, each rendered and thrown away. A page's new request replaces its waiting one. |
| rendering | `pdf_view.cpp` `paintEvent`, `requestRenders` | Moving the window to a screen with another pixel ratio left pages stretched until the next scroll. Pages are asked for again when the ratio changes. |
| leak | `pdf_view.cpp` `imagesTimer_` | A hidden tab that released its images refilled the cache from renders still queued. The queue is emptied too; showing the tab asks for any missing page (not only when the cache is empty, which left pages blank). |
| perf | `pdf_view.cpp` `onRendered` | Each rendered page repainted the whole viewport; now only its rectangle. |
| perf | `pdf_view.cpp` `paintAnnotations` | Ink paths were copied on every paint. |
| perf | `document.cpp` `pageMatrix` | Coordinate conversion loaded the page before looking at the cached matrix: every annotation reload went through every annotated page via the 12-page cache. |
| rendering | `reader_tab.cpp` | Area-annotation images in the sidebar came out half size on 2x screens. |

### Notes (`note_editor`, `note_pictures`, `math_render`)

| Kind | Where | What |
|---|---|---|
| leak / perf | `math_render.cpp` | The formula cache had no byte limit (a display formula typed in place could hold hundreds of MB) and was emptied whole at 500 entries (a note with more re-rendered all of them at each layout). Now an LRU `QCache` of 160 MB. |
| perf | `note_editor.cpp` `runAtCursor` | Walked every fragment of the note, 5–6 times per key and on every cursor move. Now only the cursor's block. |
| rendering | `note_pictures.cpp` | An empty caption's "Add a caption" hint was left behind or not drawn when the cursor moved in or out without editing. |
| rendering | `note_pictures.cpp` `startPictureDrag` | The drag image was blurry on HiDPI. |

### Library and the rest of the GUI

| Kind | Where | What |
|---|---|---|
| perf | `icons.cpp` `SvgIconEngine::paint` | Item views parsed and rendered the SVG for every row on every repaint. Now drawn from the cached pixmap at the device pixel ratio. |
| perf | `items_model.cpp` `parent`, `indexOfKey` | Linear scans of the top level, asked for every child row painted; quadratic in `reloadItems`/`selectKeys`/`sort`. Rows now carry their index. |
| perf | `items_model.cpp` `sortNodes` | The comparator rebuilt (and normalised) sort strings on every comparison. Computed once per row. |
| crash | `sync_controller.cpp` `downloadFile`, `deleteRemoteLibrary` | Detached threads posted to `this` after the window (and the controller) could be gone. Guarded with `QPointer`. |
| rendering | `library_view.cpp` | The background picture kept its old pixel ratio after moving screens; it was decoded again on every appearance change (e.g. pressing L in a PDF). |
| rendering | `item_pane.cpp` `LocationBox` | Trash icon and separator kept the old theme's colours. |
| perf | `item_pane.cpp` `TagsBox::rebuild` | Read the selected items twice. |

### Core, sync, formats (`src/core`, `src/formats`)

Also checked under ASan + UBSan (LeakSanitizer included): `test_core`, `test_sync` and the
formats tests, with no reports before or after.

| Kind | Where | What |
|---|---|---|
| perf | `sync/engine.cpp` `record` | A linear search (and erase from the middle) of the report for each object recorded: a sync was quadratic in objects, inside the pull's write transaction. Indexed by (library, type, key). First sync of 60k items: push 35.8 s → 14.9 s, pull 39.5 s → 17.6 s. |
| perf | `sync/engine.cpp` `syncFiles` | Parsed every item of the library at every sync to find attachments; now reads attachments only. |
| perf | `sync/engine.cpp` `push` | Each dirty object's JSON was copied (twice the memory on a first sync); now moved. |
| perf | `library.cpp` `putRemoteObject` | Parsed the stored object only to learn whether it existed; now `SELECT 1`. |
| perf | `library.cpp` `copyItems` | Every copied note searched for every copied key (O(K·N·size)); now each image key in the note is looked up once. |
| correctness | `util.cpp` `writeFileAtomic` | A failed flush (full disk) went unnoticed and a truncated temp file replaced config.json, reader state or an export. Now checked; the temp file is removed. |
| crash | `citation.cpp` `ordinal`, `pageRange` | `std::stoi`/`std::stol` threw `out_of_range` on long numbers (an ISBN in Edition, long page numbers): one such item made the whole bibliography fail. |
| correctness | `metadata.cpp` `downloadFile` | A PDF cut short by a full disk was accepted; now it fails and the `.part` file is removed. |

## Round 1: reported, not fixed

| Kind | Where | What, and why not fixed |
|---|---|---|
| perf | `pdf_view.cpp` `find` | Searches every page synchronously on the GUI thread (seconds on a long PDF). Needs an asynchronous search. |
| leak | `document.cpp` `text()` | Every page's extracted text is kept until `releaseMemory`. Callers hold `const PageText&` across calls, so eviction needs an API change. |
| perf | `pdf_view.cpp` `~PdfView` | Closing a tab waits for the page render in progress on the GUI thread. |
| perf | `pdf_view.cpp` `mouseMoveEvent` | Whole-viewport repaint on every move while dragging (selection, ink, move, resize). |
| perf | `render_worker.cpp`, `document.cpp` `toImage` | Each rendered page is copied twice; dark mode recomputes image rects each render. |
| rendering / perf | `rich_text_images.cpp` `drawObject` | Pictures drawn from the full-size image at each paint with bilinear filtering: aliasing on large downscales. A scaled-copy cache needs care during live resize. |
| rendering | `rich_text_images.cpp` | Formulas laid out at the app's highest DPR but drawn at the widget's: up to ~1 px misplacement on mixed-DPI setups; each formula cached twice. |
| perf | `note_html.cpp` `noteHtmlForEditor` | Each save reloads every other editor showing the note, decoding all its pictures from disk. Needs a shared image cache. |
| behaviour | `note_editor.cpp` `adaptColors` | `setUndoRedoEnabled(false)` on a theme/palette change wipes the undo history. |
| perf | `TagSelector`, `ItemNotesPanel`, `LocationBox` | Rebuilt on any item change (including note autosave), hidden ones too. |
| behaviour | `sync/change_watcher.cpp` `run` | The reconnect back-off is never reset after a successful reconnect: after one outage, every later error waits up to 300 s. |
| robustness | `server/main.cpp` `cmdServe` | An exception between starting the watcher thread and joining it ends in `std::terminate`. |
| perf | `sync/server_api.cpp` `LoginLimiter::failed` | With more than 10k live addresses the whole map is scanned on each failure and still grows (PBKDF2 limits the rate). |
| perf | `cli/extra_commands.cpp` `import` | Each imported item commits on its own; one transaction would hold the write lock long enough for the GUI's 15 s busy timeout. |
| perf | `library.cpp` `writeItem` | The INSERT is prepared again for each tag and collection (no statement cache). |
| perf | `hash.cpp` `pbkdf2Sha256` | The HMAC pads are recomputed in each of the 120k iterations: about twice the CPU per login. |

## Round 2: text from a group or a web page shown as rich text

Qt shows a label's or tooltip's text as HTML when it looks like HTML, and HTML's `<img>`
reads the file it names (on Windows a `//host/share/…` name makes an SMB connection).
`<img src="/dev/zero">` ends in `std::bad_alloc`. Text that others control reached such
places: a library's name (status bar, every sync), an item's dates (shown on selection),
an annotation's page label and tags, titles in tooltips, a web page's title saved from
the browser (status bar), and `<img>` in a synced note.

| Kind | Where | What |
|---|---|---|
| crash / file read | `note_editor.cpp` `NoteTextEdit::loadResource` | A note's `<img>` naming a local or relative path was read from disk. Not read now (the note's own pictures are resources; web addresses are not read either way, and pasted ones are still adopted). Test: "notes: a synced note's <img> naming a file is not read". |
| crash / file read | `main_window.cpp` status bar; `item_pane.cpp` dates; `annotation_sidebar.cpp` tags | Plain-text labels. |
| crash / file read | `annotation_sidebar.cpp` header | The page label (and type) are escaped in the card's rich-text header. |
| crash / file read | tooltips: `items_model.cpp` titles, `tab_widget.cpp` tabs, `sync_history.cpp`, `metadata_progress.cpp`, `note_editor.cpp` links, the sync button | Through `plainTip()` (`common.h`): text that looks like HTML is escaped. |

Reported: pasting HTML (`setHtml` on a parentless `QTextDocument` in `note_editor.cpp`
paste paths) still lets Qt read local files named by `<img>` in the clipboard's HTML.
That is the user's own clipboard, so it was left. Message boxes that quote titles
(`QMessageBox` auto-detects rich text) were not all checked; they need the user to act
first.

Not finished: the second-pass review was cut short. Its other high-ranked lead,
attachments synced by others being opened with the system's handler with no check of the
file type, is how Zotero behaves too and is a design decision rather than a bug; it is
noted here for review.

## Round 3: from the sanitizers and the second pass

The whole suite under ASan + UBSan + LeakSanitizer (GUI included, offscreen): no error
and no leak in OurNotes' own code. In MicroTeX, which OurNotes builds and patches:

| Kind | Where | What |
|---|---|---|
| leak | MicroTeX `macro_impl.h` (patched in `cmake/PatchMicroTeX.cmake`) | Array environments (matrix, align, gather, multline…) held their rows in a raw pointer until the end: one that threw while parsing, as one typed in place does at most keys, leaked ~350 bytes per render. Owned from the start. |
| memory error | MicroTeX `latex.cpp` (patched) | `asprintf`'s string freed with `delete`. |
| crash | `util.cpp` `safeFilename` | A name over 200 characters whose "extension" was over 200 (a synced attachment's filename, read at every sync) made `200 - ext.size()` wrap round and read past the end. Test in `test_util.cpp`. |
| crash | `core/json.h` `parseExternalJson` | JSON nested ~100k deep parses, then overflows the stack when copied or dumped. Requests to the sync server and the connector, and the server's responses to the app, refuse nesting over 256. Test in `test_util.cpp`. (Commit d72c4d3 says "not yet built or tested": it was, afterwards, and passes.) |
| perf | `items_model.cpp` `reload`, `library.cpp` `itemRows` | Each reload (after every change, a note's autosave too) read and parsed every attachment's JSON to see whether its file is there. `itemRows` now gives an attachment's filename and path. 5,000 papers with a PDF each: 227 ms → ~100 ms per reload. |

Left: a 12-byte allocation MicroTeX makes once at start (fonts), never freed.
