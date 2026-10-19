# freePen

freePen is an open-source reference manager in the spirit of Zotero: collect papers,
read and annotate PDFs, write notes, cite, and keep everything in sync — through
**your own server** (`freepend`) or **a plain folder** (USB drive, NAS, network share).

It is written in C++17 with Qt 6 and MuPDF, and targets Linux, macOS and Windows.

## Features

- **Library** — personal and group libraries, nested collections, tags with colours,
  related items, duplicate detection and merging, trash. All 37 Zotero item types and
  their fields; data is stored in Zotero's API JSON shape.
- **Zotero import** — copies everything from a Zotero data directory: libraries
  (including groups), collections, items, creators, tags, relations, notes (with embedded
  images), attachments and their files, annotations (highlights, underlines, notes,
  images, ink), tag colours, reading positions and CSL styles. Zotero's files are only
  read. Re-running the import picks up items changed in Zotero since.
- **PDF reader** — continuous scrolling, zoom/fit, thumbnails, outline, links, find.
  Annotation tools: highlight, underline, sticky note, text box, area (image), ink and
  eraser, eight colours, comments, move/resize. Annotations are stored like Zotero's
  (PDF user-space coordinates) and imported Zotero annotations line up exactly.
  Annotations already inside a PDF are shown read-only; *File → Export PDF with
  Annotations* writes yours into a copy of the file.
- **Notes** — rich-text notes (headings, lists, quotes, links, colours, pasted images)
  saved as Zotero-compatible note HTML; add selected text or annotations to a note.
- **Adding items** — by DOI, ISBN, arXiv ID or PMID (arXiv PDFs are downloaded), by
  dropping PDFs (metadata is found from the identifiers or title in the file, with a
  progress window), or by importing BibTeX / RIS / CSL JSON. PDF text is read locally;
  only the identifier or title is sent to the public Crossref, arXiv, Open Library or
  PubMed APIs (no account or API key needed).
- **Citing** — export BibTeX, RIS, CSL JSON; bibliographies and in-text citations in
  APA, Chicago (author-date), IEEE, MLA, Harvard, Nature and Vancouver.
- **Search** — title/creator/year, all fields and tags, or everything including the
  full text of PDFs (indexed in the background with SQLite FTS5).
- **Sync** — two-way, per object, with attachment files; last writer wins and the losing
  version is kept in *Tools → Sync Conflicts*. Works with a `freepend` server (HTTP/HTTPS,
  multiple users, shared libraries) or with a folder.
- **Command line** — `freepen-cli` for import/export, citing, adding items, indexing and
  syncing on headless machines.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.21, Qt ≥ 6.5 (Widgets, Svg), OpenSSL ≥ 3
(for HTTPS). MuPDF is built from source automatically on Linux and macOS; SQLite is
downloaded at configure time. nlohmann/json, cpp-httplib and doctest are vendored.

### Linux

```sh
sudo apt install build-essential cmake libssl-dev libgl1-mesa-dev   # Debian/Ubuntu
# Qt 6 from your distribution (qt6-base-dev libqt6svg6-dev) or the Qt installer, e.g.
#   pip install aqtinstall && aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O ~/Qt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=~/Qt/6.8.3/gcc_64
cmake --build build -j
ctest --test-dir build          # set FREEPEN_NO_NETWORK=1 to skip online lookups
./build/src/gui/freepen
sudo cmake --install build      # installs freepen, freepen-cli, freepend, .desktop file
```

### Installing on Linux (per user)

```sh
scripts/install-linux.sh                # build + install, menu entry and desktop launcher
scripts/install-linux.sh --uninstall    # remove it again (your library is kept)
```

The script builds a release copy, installs it with its own Qt runtime in
`~/.local/opt/freepen`, links `freepen`, `freepen-cli` and `freepend` into `~/.local/bin`,
and adds the icon, an application-menu entry and a launcher on the desktop. Options:
`--qt DIR`, `--prefix DIR`, `--no-desktop-shortcut`, `--no-build`.

### macOS

```sh
brew install cmake openssl@3 qt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j
open build/src/gui/freepen.app
```

### Windows (Visual Studio 2022)

```powershell
vcpkg install mupdf openssl            # MuPDF cannot be built with make on Windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
```

Other build options: `-DMUPDF_ROOT=<prefix>` uses an existing MuPDF build;
`-DFREEPEN_BUILD_GUI=OFF -DFREEPEN_BUILD_CLI=OFF` builds only the sync server (no Qt, no
MuPDF); `-DFREEPEN_WITH_OPENSSL=OFF` builds without HTTPS.

> Status: developed and tested on Linux (Ubuntu 22.04, GCC 11, Qt 6.11). The macOS and
> Windows builds are set up (code paths, CMake, CI workflow in `.github/workflows`) but
> have not been run yet.

## Getting started

On first start freePen offers to import an existing Zotero library (it looks for
`~/Zotero` or the data directory configured in Zotero). You can also import later with
*File → Import from Zotero…* or:

```sh
freepen-cli import-zotero ~/Zotero
```

The interface is scaled to 150% by default; change it in *Preferences → General →
Interface scale* (100–300%, applied on restart). A `QT_SCALE_FACTOR` set in the
environment takes precedence.

Data lives in `~/freePen` (database `freepen.sqlite`, files in `storage/<KEY>/`), like
Zotero's data directory; change it in *Preferences → General* or with
`FREEPEN_DATA_DIR`. Settings and sync tokens are in `~/.config/freepen/config.json`
(macOS: `~/Library/Application Support/freePen`, Windows: `%APPDATA%\freePen`).

## Sync

**With a server.** On any machine you control:

```sh
freepend useradd alice                     # prompts for a password
freepend serve --port 8642                 # add --cert/--key for HTTPS, or use a reverse proxy
```

Then in freePen: *Preferences → Sync → Add Server…*, enter the URL, user name and
password (only a sign-in token is stored). Libraries can be shared between accounts with
`freepend share <library-uid> bob --role write`. See [docs/server.md](docs/server.md) for
TLS, systemd and backups, and [docs/sync-protocol.md](docs/sync-protocol.md) for the API.

**With a folder.** *Preferences → Sync → Add Folder…* and pick a directory on a USB drive
or network share. Every computer that syncs with the same folder gets the same libraries.
Use it from one computer at a time (it is not meant for live sharing by Dropbox-style
tools, which can corrupt databases).

Sync runs automatically after edits and every 15 minutes (configurable), or with the sync
button. Attachment files are transferred during sync or, optionally, when first opened.

## Command line

```text
freepen-cli info | libraries | collections | items [--search TEXT]
freepen-cli add paper.pdf 10.1038/nature14539 arXiv:2301.08243
freepen-cli import refs.bib            freepen-cli export --format ris -o out.ris
freepen-cli cite KEY... --style ieee   freepen-cli index
freepen-cli add-server https://host:8642 --user alice    freepen-cli sync
```

## Architecture

```
src/core     Qt-free library: SQLite database (items/collections/settings as JSON with
             derived columns), FTS5 full-text index, Zotero importer, sync engine
src/sync     sync storage (also used for folder targets), HTTP and folder transports,
             HTTP API of the server
src/server   freepend, the sync daemon (needs only the C++ standard library + core)
src/pdf      MuPDF wrapper: rendering, text geometry, search, outline, annotations
src/formats  BibTeX/RIS/CSL JSON, DOI/arXiv/ISBN/PMID lookup, citation styles
src/cli      freepen-cli
src/gui      Qt 6 desktop application
tests/       doctest suites for every module (GUI tests use Qt Test, offscreen)
resources/   item type schema, application icon (SVG source + generated PNG/ICO/ICNS)
tools/       gen_schema.py (schema from a Zotero database), render_icons (icon assets)
scripts/     install-linux.sh
```

Every object (item, collection, setting) carries the server version it was last synced
at and a local revision counter; deletions leave tombstones. A sync pulls changes since the
last known library version, reconciles them with local edits, transfers files by MD5, and
pushes local changes with optimistic concurrency.

## Known limitations

- Citation styles are built in; the CSL files copied from Zotero are kept in
  `~/freePen/styles` but not yet interpreted.
- EPUB and HTML snapshots open in the system viewer; annotations are PDF-only.
- Zotero feeds, saved searches and its web sync are not imported/supported.
- Notes are rewritten as clean HTML when you edit them; Zotero-specific markup
  (embedded citation data) in edited notes is reduced to its visible text.

## License

freePen is free software under the GNU Affero General Public License v3 (see `LICENSE`),
the same license as Zotero and MuPDF. The item type schema is derived from Zotero.
Third-party code: MuPDF (AGPL-3.0), Qt (LGPL-3.0), SQLite (public domain),
nlohmann/json, cpp-httplib and doctest (MIT).
