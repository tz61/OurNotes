# Audit: memory, performance, rendering (October 2026)

An audit of the code at 0.2.1 for memory leaks, performance problems and rendering
errors, and then for anything else suspicious. Each finding is either **fixed** (the commit
is on this branch) or **reported** (with why it was left).

How it was checked: Qt 6.8.4 and MuPDF 1.28.5 (conda-forge), the full build, and
`ctest` with the GUI tests offscreen, before and after each round of fixes. All 7 test
programs pass before and after. Also: the suite under ASan + UBSan + LeakSanitizer,
libFuzzer on the import parsers and string helpers, timings of the hot paths changed,
and each finding of the later rounds given to a skeptic told to refute it.

In short: 59 findings fixed (some rows cover several places), each with the suite green
after it, and new tests where a test could show it. The ones that mattered most:

- **Crashes from what others sync.** Text that looks like HTML (a library's, tag's,
  collection's name, a title, a date, a note's `<img>`) was shown as rich text, which reads
  the files `<img>` names: `/dev/zero` took every member of a group down (rounds 2, 5). A
  long attachment filename and deeply nested JSON crashed the sync too (round 3).
- **Sync.** A file conflict made a new conflicted copy at every sync, forever (round 5);
  a target restored from a backup made the next sync push the whole library (round 6);
  a sync was quadratic in objects (round 1: 60k items, 36 s → 15 s).
- **Memory.** The formula cache had no size limit; the link preview kept a 10 MB tile per
  page wheeled through; MicroTeX leaked at each half-typed matrix (rounds 1, 3, 4).
- **Speed while typing or scrolling.** Icons parsed per row per paint, link tints made per
  paint (up to 28 ms a page), the item list's reload after each autosave (227 → 100 ms),
  every hidden PDF tab reloading its annotations (rounds 1, 3, 4, 5).
- **CLI.** A 20 KB picture overflowed the stack (`std::regex`); a quote not in a PDF could
  be "found" across a page break and a wrong highlight saved (rounds 4, 5).

Reported but not fixed, with the reasons, at the end of round 1, in round 5's list and in
round 7.

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
| robustness | `server/main.cpp` `cmdServe` | An exception between starting the watcher thread and joining it ends in `std::terminate`. |
| perf | `sync/server_api.cpp` `LoginLimiter::failed` | With more than 10k live addresses the whole map is scanned on each failure and still grows (PBKDF2 limits the rate). |
| perf | `cli/extra_commands.cpp` `import` | Each imported item commits on its own; one transaction would hold the write lock long enough for the GUI's 15 s busy timeout. |
| perf | `library.cpp` `writeItem` | The INSERT is prepared again for each tag and collection (no statement cache). |

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
| crash / file read | tooltips: `items_model.cpp` titles, `tab_widget.cpp` tabs, `sync_history.cpp`, `metadata_progress.cpp`, `note_editor.cpp` links, the sync button | Through `asPlainText()` (`common.h`): text that looks like HTML is escaped. |

Reported: pasting HTML (`setHtml` on a parentless `QTextDocument` in `note_editor.cpp`
paste paths) still lets Qt read local files named by `<img>` in the clipboard's HTML.
That is the user's own clipboard, so it was left. Message boxes quoting a collection's,
tag's, note's or library's name, an item's title, or an error from the sync server or a
PDF (`QMessageBox` takes text that looks like HTML as rich text too) go through
`asPlainText()` as well (`plainTip()` renamed, as it is no longer only for tooltips).

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
| behaviour | `sync/change_watcher.cpp` `run` | The reconnect back-off was never reset: after one outage, every later error waited up to 300 s before the next try. It starts over when the try that failed had watched a while (half a held request's time: a laptop waking, a proxy dropping held requests); a target that fails at once, even one that answers and then fails each held request, is still asked later and later. Test in `test_http.cpp`. |
| perf | `hash.cpp` `pbkdf2Sha256` | The padded password was hashed again in each of a login's 120k iterations. Hashed once; each iteration copies the two states. 211 ms → 113 ms per login or `useradd` (output unchanged: RFC 7914's 80,000-iteration vector added to the tests). |
| perf | `library.cpp` schema 4, `tags()` | The tag list (rebuilt after every item change, a note's autosave too) looked up each tag's item in `items`, whose rows carry the item's whole data: a deep tree to walk for each. An index `items(library_id, key, trashed)` answers it: `tags()` 38 → 24 ms, the tag selector 51 → 37 ms (5,000 papers, 15,000 tags on them). Older versions of the app open the database as before. |

Fuzzed (libFuzzer, ASan + UBSan, leaks checked, 20 minutes, seeded with the formats
tests' samples): `importBibtex`, `importRis`, `importCslJson`, `detectFormat`,
`latexToUnicode`, and what they import written back out (`exportBibtex`, `exportRis`,
`exportCslJson`, APA, Chicago and IEEE citations), ~970,000 inputs; `htmlToText`,
`noteTitle`, `yearOf`, `normalize`, `searchTokens`, the date helpers, `safeFilename` and
`parseItemLink`, ~4.9 million inputs. No crash, no memory error, no leak.

Left: a 12-byte allocation MicroTeX makes once at start (fonts), never freed.

## Round 4: the reader code that came in meanwhile (048f1ee..786d741)

Reviewed, then each finding given to a skeptic told to refute it (six of eight held).

| Kind | Where | What |
|---|---|---|
| leak | `pdf_view.cpp` `LinkPreview::tileFor` | The link preview kept a rendered tile (≈8–10 MB at 2x) for every page wheeled through, until the next preview. Now the pages shown and one each side; none while hidden. |
| perf | `pdf_view.cpp` `paintLinks` | Each link's tint (an image copy, a pixel scan, a fill, two composited draws) was made again on every paint, at every step of a scroll: 6–28 ms a page for 50–400 links. Made once per rendered image, kept with it. |
| perf | `LinkPreview::tileFor` | Rounding put the area needed a hair past the tile at a page's edge, so ~12% of positions rendered the tile again at every paint. |
| rendering | `LinkPreview::paintEvent`, `tileFor` | Tiles were rendered to whole pixels but drawn into fractional rects without smoothing: columns dropped, text off by a pixel and shimmering while scrolling. Snapped to device pixels as the main view does. |
| behaviour | `LinkPreview::zoom` | A preview begun outside [0.4, 4] jumped to the limit at the first Ctrl+wheel, zooming the wrong way. |
| correctness | `cli/annotation_commands.cpp` `passageAcross` | A quote not in the PDF could be "found" across a page break from a phrase mid-page and any matching word on the next (exit 0, a wrong highlight saved and synced). Both halves must now be whole words near the break. Test in `test_cli.cpp`. |

Refuted: `pictureRects()` running page content on the GUI thread (cached per page, once
a document); the preview's leave timer restarting on each move (as comment bubbles do,
on purpose).

## Round 5: a second sweep, four fresh lenses (lifecycles, sync under load, drawing, the CLI)

Twenty candidates; the five ranked highest were each given to a skeptic (all held).
The GUI, CLI, sync and core tests under ASan + UBSan + LeakSanitizer again with these
fixes: clean (the 12 bytes MicroTeX keeps from its start aside).

| Kind | Where | What |
|---|---|---|
| leak / sync | `core/sync/engine.cpp` `syncFiles` | A file conflict imported the local file as a "conflicted copy", then asked for the remote one; when that was skipped (files on demand) or failed, nothing recorded the conflict as handled, so every later sync imported another copy, uploaded and synced to every computer. The kept file's checksum is recorded. |
| crash | `item_pane.cpp` tag chips, location and notes tooltips, attachment "Added"; `library_view.cpp` Merge Items; `main_window.cpp` Delete Library | The same rich-text `<img>` path as round 2, in places it missed. |
| crash | `cli/agent_commands.cpp` `notePictures`, `text` | `std::regex` (libstdc++ recurses per character) read `<img>` tags: a `data:` picture of ~22 KB or more crashed `add-note`, `edit-note`, `add-image` and `text` with a stack overflow. Read by hand now (checked equal to the regexes on 400k random tags). Test in `test_cli.cpp`. |
| perf | `reader_tab.cpp` | Every open PDF tab, hidden ones too, reloaded its annotations and notes on any item change (a note's autosave too). A hidden tab now does it when shown. |
| perf | `reader_tab.cpp`, `pdf_view.cpp` `requestThumbnails` | Once thumbnails were shown, every restart of rendering (a tab shown again) rendered all of them again; now only those missing (all again on a colour or screen change). |

Found, not verified (left for review; each is a lead, not a confirmed defect):

- `sync/store.cpp` `Store::changes`, `Store::push`: every row's JSON parsed while holding the server-wide mutex; each pushed object's stored JSON parsed only to compare versions.
- `core/sync/engine.cpp` `syncFiles`: every sync downloads and parses the full file list of every library, even when nothing changed.

## Round 6: the leads of round 5, verified

Eight leads given to skeptics; six held and are fixed, two are not worth their code
(`Store::changes`/`push` parsing under the server's lock: bounded by page and batch
limits; `syncFiles` asking for the file list at each sync: one small request).

| Kind | Where | What |
|---|---|---|
| sync | `core/sync/engine.cpp` `pull` | After a remote reset (a restored backup), `continue` in the `do … while` ended the pull at once: an editable library pushed every object (each the server had came back a conflict), a read-only one failed with 403, and an empty one pulled nothing, until the next sync. The first page is asked for again from version 0. |
| correctness | `core/util.cpp` `htmlUnescape` | `&#xD800;` or `&#x110000;` became bytes that are no UTF-8: one such note made `items --json` fail for the whole library (nlohmann throws on them). U+FFFD instead, as browsers do. Test in `test_util.cpp`. |
| UB | `cli/agent_commands.cpp` `base64Decode` | A signed int shifted left past its range on any `data:` picture longer than a few bytes. Only the pending bits are kept. |
| rendering / data | `note_editor.cpp` `adaptColors` | From a light theme to a dark one, links (coloured with the palette's link colour) were taken for text of a colour of their own and shown in the text colour. They take the new palette's link colour. |
| rendering | `item_pane.cpp` `AutoTextEdit::sizeHint` | Lines are laid out at their height rounded up, measured rounded to nearest: a pixel short a line, so long abstracts, titles and comments lost their last lines. |
| rendering | `item_pane.cpp` `RelatedBox` | A related item's long title made the Info page wider than the pane, cutting off every field's right side. |

## Round 7: the rest of round 5's leads

| Kind | Where | What |
|---|---|---|
| leak | `search_field.cpp` `showList` | The find field's history list was made a child of the main window: every closed tab whose find field had shown it left it behind (up to 1000 entries) for the session. It goes with its field. |
| rendering | `search_field.cpp` `updateCaseButton` | The Aa toggle was drawn 18×16 for a 16×16 place: scaled twice, out of proportion, blurred. Square now. |
| rendering | `metadata_progress.cpp` | A found title kept the text colour of the theme it was found in: invisible after switching light ↔ dark with the dialog open. It takes the list's own. |

Left, as a decision for the owner: `ournotes-cli cite` and `export` given keys leave
out, without a word and with exit 0, those not in the library or not regular items (a
typo, a note): an agent gets part of what it asked for. Failing instead would change
what scripts can rely on.

Not defects after all: `storage_window.cpp` already drops an earlier Refresh's answers
(a generation counter), and `index`/`empty-trash` reading every item once is a one-off
command's cost.

Also: the test "sync: another computer's push is pulled by itself, at once" failed once
under `ctest -j2` (passing alone): it counted the syncs finished as soon as the pulled
title was there, while the sync's end is told a moment later. It waits for it now.

## Second check: main's commits one by one (leads, not yet verified)

Main's history before 0.2.1 (51 commits, version bumps aside) read commit by commit for
bugs a commit brought in that are still in the code. Four of the six ranges were done by
11am ET; none of these was given to a skeptic or fixed yet, so each is a lead to check.

- **high**, correctness (from a87c3e1), `src/cli/agent_commands.cpp:681`: edit-note wipes a note when its text argument is empty or starts with '-'. `ournotes-cli edit-note NOTEKEY "- first point"` (a Markdown bullet list, or text that starts with '---' front matter or a horizontal rule), or `edit-note NOTEKEY "$TEXT"` with TEXT empty, run from an agent or script whose stdin is empty or /dev/null. Args treats any argument longer than one character that starts with '-' as a flag, so the text is dropped. An empty argument also counts as missing. In both cases contentFrom reads stdin, gets "", and markdownToNoteHtml("") gives `<div data-schema-version="9"></div>`. Unlike add-note, edit-note has no emptiness check, so updateItem replaces the note with an empty one. The command prints the key and exits 0, and the next sync sends the empty note everywhere. I reproduced this with the built CLI: the note's HTML became `<div data-schema-version="9"></div>` and the exit code was 0. In a terminal the same call hangs waiting for input instead. With --append it silently adds nothing.
- **high**, correctness (from 96839d2), `src/gui/note_html.cpp:103`: tableHeadersForEditor duplicates the rest of a note when a header row holds a table with a header row. The row regex (line 95) stops at the first </tr> it finds, which can be the </tr> of a table nested inside a header cell. The header-row loop then takes in the nested table's start. The next <table> match in globalMatch is that nested table, which starts before `last`, so `head - last` is negative. html.mid(last, negative) returns everything from `last` to the end, and the final `out + html.mid(last)` appends it a second time. I reproduced this against the current build. One case: paste the Markdown text "| a | b |\n|---|---|\n| 1 | 2 |" with the cursor in a header cell of an existing note table. It is saved as <th>Head<table><tr><th>a</th>...</table></th>. Open the note again and type one letter: the stored note now has the body row ("body1") and the trailing paragraphs twice, and the H2 header cell has been pushed out of the table. Each later open and edit adds more copies, and the corrupted note syncs. A direct HTML case: <table><tr><th>A<table><tr><th>B</th></tr><tr><td>c</td></tr></table></th></tr><tr><td>d</td></tr></table> becomes 219 characters from 106. HTML pasted from the web with that nesting is duplicated at paste time, because insertFromMimeData also calls this function. When the nested table's first row uses <td>, the outer table's header row is lost instead: it is saved as <td><strong>.
- **medium**, correctness (from 53d4437), `src/gui/app_context.cpp:36`: Changes another program makes while the app's own background writes run are dropped: no refresh, and no auto-sync. The full-text indexer calls beginOwnWrites() for its whole run. That run starts 8 s after launch, 10 s after every item added in the GUI, and on Rebuild Full-Text Index, and indexing a freshly imported library takes minutes to hours. File downloads and syncs open the same window. While ownWrites_ > 0, each 2 s tick returns without recording the new data_version. endOwnWrites() then sets dataVersion_ to the current value, so a `ournotes-cli` edit made in that window (for example, retitling an item while the indexer runs) counts as the app's own write. After an indexer run or a download nothing calls notifyRefresh(), so the list, item pane and tabs keep showing the old data until some unrelated change. changedElsewhere() is never emitted for the edit, so no auto-sync follows. After a sync the views do refresh, but the CLI's edit still waits for the interval sync (15 min by default), not the 20 s auto-sync the commit promises. With no sync target, the views stay stale indefinitely. The GUI test only covers a CLI write made outside these windows.
- **medium**, correctness (from 6f2c433), `src/core/library.cpp:1156-1165`: Items copied to another library get permanently file-less attachments when the source file is not on this computer (files on demand). Set Preferences → Sync to 'Download attachment files when opened' (on_demand), so synced PDFs are not stored locally until opened. Drag such items from My Library onto a group library or one of its collections. copyItems creates each attachment under a new key, but fs::is_regular_file(src) is false, so the file copy is skipped without a word. The sync server only has the file under the old key in the source library, so syncFiles never uploads one for the new key and nobody can download it. Opening the copy, here or for any group member, tries downloadFile for the new key and the server answers 404. The source's file was available on the server the whole time, yet the copy says it carries its PDF (README: 'attachments (and files)').
- **medium**, correctness (from b63c88a), `src/gui/note_editor.cpp:154`: Pasted code containing $ (R df$x, shell "$A/$B", PHP $a.$b) is turned into formulas. Before b63c88a, plain text pasted into a note stayed as it was unless it looked like Markdown in a Markdown note. Now any plain-text paste that matches latexInText is converted, and formulasFromText runs the same regex over every block of pasted HTML, <pre> and <code> included. The regex only rejects a $ that comes right after \ or $, and a closing $ followed by a digit. So R code pasted from a terminal, an .R file or RStudio, such as `plot(d$x, d$y)`, shows as "plot(d" + formula "x, d" + "y)" and is saved as <span class="math">$x, d$</span>. `df$age + df$height` gives the formula "age + df", `cp "$SRC/$NAME" out` gives "SRC/", and `echo $first.$last;` gives "first.". I checked these strings against the regex with PCRE. A StackOverflow R answer pasted as HTML gets the same treatment inside its code block.
- **low**, correctness (from a87c3e1), `src/core/util.cpp:519-526`: Markdown to note: a lone '**' is deleted from the text. `ournotes-cli add-note KEY "Runtime is O(n**2) in the worst case."` is saved as `<p>Runtime is O(n<em></em>2) ...</p>` and reads "O(n2)" (verified with the built CLI). Likewise `"*args and **kwargs"` comes out as "*args and kwargs". A '**' with no closing '**' fails the strong branch. It then falls into the em branch: line[i+1] is '*', which passes the `!= ' '` test, and find('*', i+1) returns i+1 itself. The result is an empty <em></em> that consumes both asterisks. The text an agent wrote is changed silently in the saved, synced note.
- **low**, rendering (from fb29ac1), `src/gui/main_window.cpp:227-230`: X11 above 100%: a full-height normal window reopens maximized. On X11 at a scale other than 100% (e.g. KDE at 150%), the user leaves the window un-maximized but as tall as the work area: quick-tiled to the left half (Meta+Left), or resized to full height, then quits. At the next start, restoreGeometry gives the client height, roughly availableGeometry().height() minus the title bar (about 20-30 logical px). The 'too big' test compares it with availableGeometry().height() - 40, so the window counts as too big. It is set to a 5% inset geometry and maximizeWhenShown_ = true, so it opens maximized over the whole screen instead of half-width. This repeats at every start, because a maximized window saves maximized.
- **low**, crash (from 6f2c433), `src/core/library.cpp:1162-1163`: A failed file copy during a drag to another library throws out of dropEvent and terminates the app, leaving a partial copy. Drag several items with large PDFs onto another library while the disk is nearly full, or while a source file cannot be read (EACCES). fs::copy_file throws std::filesystem_error. Nothing in CollectionTree::dropEvent, the drag's nested event loop or main() catches it, so Qt rethrows it out of the event loop and std::terminate aborts the app. The items copied before the failure stay in the target library, because copyItems runs no transaction (BatchGuard only batches notifications), and the failing file is left half-written. Any note text still waiting in an editor's 700 ms save timer is lost.
- **low**, perf (from 10a1816), `src/sync/store.cpp:752-764`: GET /api/v1/status parses every object of the caller's libraries while holding the store-wide mutex. Any authenticated user opening Tools → Storage and Sync Status, or pressing Refresh, makes ournotesd run json_extract(data, '$.itemType') over every item row of every library they belong to, with GROUP BY, while holding mutex_. The objects_version index does not help, and every row's full JSON is parsed. For a 60k-item group library with annotations that is on the order of a hundred MB of JSON per call. Every other user's push, pull and file request waits for it, and a member pressing Refresh repeatedly (or scripting the endpoint) can stall the server for everyone. This is not the 'bounded by page and batch limits' case that round 6 set aside: nothing bounds this query.
- **low**, correctness (from b63c88a), `src/gui/note_editor.cpp:1913-1934`: Plain text with LaTeX pasted loses the heading or formatting where it lands. Plain text that contains $…$ now goes through textDocument(markdown=false): setPlainText, then insertFragment(QTextDocumentFragment(&pasted)). That fragment is not imported from plain text, so its text keeps the pasted document's empty character format instead of the cursor's. On an empty line, the code also runs first.setBlockFormat(pasted.begin().blockFormat()), which is a default block format. Example: type "## " to start a heading, then paste "Energy $E=mc^2$" from a text editor. The line becomes an ordinary paragraph and is saved as <p>. Pasted into a non-empty heading, or into red or bold text, the text arrives body-size, not bold and in the default colour, and the formula ignores the text colour. The same paste without a $ falls through to QTextEdit::insertFromMimeData and keeps the heading and formatting.
- **low**, perf (from b63c88a), `src/gui/note_editor.cpp:154`: The LaTeX regex backtracks quadratically on an unpaired $$, and every plain-text paste now runs it. The display branch \$\$([^$]*\S[^$]*)\$\$ tries every split point of a $-free stretch after a $$ that is never closed, so the cost grows with the square of the stretch. b63c88a now runs latexInText().match(source->text()) on every plain-text paste. Before, the regex ran only for Markdown-looking pastes in Markdown notes. Example: paste a long plain text, such as a paper's text or a log, with one "$$" near the start ("Price tier: $$") and no other $. Timed with PCRE2 JIT (grep -P, same engine as QRegularExpression): 40k chars 0.19 s, 80k 0.72 s, 160k 2.85 s, four times longer each time the length doubles. The GUI is frozen for that time, and it is about 11 s at 320k.
- **low**, rendering (from 96839d2), `src/gui/rich_text_images.cpp:189`: A formula on a cell's second or later line is drawn over the line above it. In a table, FittedImageHandler::geometry caps how far a formula reaches above its line at m.ascent - cellPadding and draws the rest above its box (g.top = ascent - m.ascent < 0). That spare height is meant to go into the cell's top padding, but the cap applies to every line in the cell, not only the first. Example: a cell with "gyp<br>$\begin{pmatrix}a\\b\end{pmatrix}$" in the default offscreen font. The formula's top sits 3.5 px above the bottom of line 1, while line 1's descent is only 2.8 px, so it overlaps the descenders of g, y and p. For \displaystyle\frac{a}{b} the overlap is 2.5 px and for x^2 it is 1.9 px. The same formula in a plain paragraph overlaps by 0 px.
