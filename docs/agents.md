# Working with an OurNotes library from an agent or a script

This is for AI agents (Claude Code, Codex, and the like) and scripts that read or change
someone's OurNotes library: summarise the papers in a collection, rename titles, write
notes, tag and file items. `ournotes-cli guide` prints this page.

## The one rule: go through `ournotes-cli`

Change the library only with `ournotes-cli`. Never write to `ournotes.sqlite` or into
`storage/` yourself:

- every change made through `ournotes-cli` is marked for the next sync, and goes to the
  user's other computers; a change written straight into the database is not, and is lost
  or overwritten later;
- the search columns, the full-text index and the files' checksums stay right.

Reading the files under `storage/` (a PDF, say) is fine; `ournotes-cli text` does it for
you.

## Where OurNotes is installed

**Run `ournotes-cli where` first**: it prints this computer's paths (the program, the
app, this guide, the data folder, the settings file, the sync target, the libraries).

If you found this file as `AGENTS.md`, the programs are beside it:

| installed with | `AGENTS.md` | `ournotes-cli` |
|---|---|---|
| Linux, `scripts/install-linux.sh` | `~/.local/opt/ournotes/AGENTS.md` | `~/.local/opt/ournotes/bin/ournotes-cli`, linked as `~/.local/bin/ournotes-cli` |
| Linux, the `.tar.gz` | at the top of the unpacked folder | `bin/ournotes-cli` in that folder |
| Linux, the AppImage | inside it | inside it: use the `.tar.gz` of the same version, or `--appimage-extract` it and run `squashfs-root/usr/bin/ournotes-cli` |
| macOS, the `.dmg` | `/Applications/OurNotes.app/Contents/Resources/AGENTS.md` | `/Applications/OurNotes.app/Contents/MacOS/ournotes-cli` |
| Windows, the `.zip` | `OurNotes\AGENTS.md` where it was unzipped | `OurNotes\bin\ournotes-cli.exe` |

Wherever it is installed, the library and the settings are in the same places:

| | |
|---|---|
| the library | the data folder the app uses (`~/OurNotes` unless changed in the app): `ournotes.sqlite`, and the attachments' files under `storage/`; `--data-dir DIR` or `OURNOTES_DATA_DIR` for another |
| settings, sync targets | `config.json` in `~/.config/ournotes/` (macOS: `~/Library/Application Support/OurNotes/`, Windows: `%APPDATA%\OurNotes\`) |

`ournotes-cli help` lists every command; `ournotes-cli guide` prints this page, followed by
what `where` says.

## What a library holds

- **Libraries** have an ID and a name (`My Library`, `Course`, ...). `--library` takes either.
- **Collections** nest. Name one by its path from the top, the names joined by `/`:
  `FA26/CS8803`. With several libraries, put the library's name first when the path alone
  is not enough (`Course/FA26/CS8803`); a collection's 8-character key works too. Case
  does not matter. If a path could mean several collections, the command says which and
  stops.
- **Items** have an 8-character key (`ABCD2345`). A regular item (a paper, a book, a web
  page...) can have child **notes** and **attachments** (a PDF, a link); a PDF can have
  **annotations** (highlights, comments). Items are in Zotero's form: `itemType`, `title`,
  `creators`, `date`, `abstractNote`, `url`, `DOI`, `tags`, `collections`, ...
- Notes are HTML (Zotero's note HTML); `add-note` takes Markdown and makes it so.

## Reading

```sh
ournotes-cli libraries --json
ournotes-cli collections --json            # every collection, with its path and item count
ournotes-cli items --collection "FA26/CS8803" --json
ournotes-cli items --search "diffusion" --json        # all libraries; --library to narrow
ournotes-cli show ABCD2345 --json          # the item, its notes (HTML and text), its attachments
ournotes-cli text ABCD2345 --pages 15      # the item's PDF as text, "[page N]" before each page
ournotes-cli text NOTE2345                 # a note as text
ournotes-cli annotations ABCD2345 --json   # highlights and comments in its PDFs
ournotes-cli export ABCD2345 --format csljson         # or bibtex, ris
ournotes-cli cite --collection "FA26/CS8803" --style apa
```

`items --json` gives, for each top-level item: `key`, `library`, `libraryId`,
`itemType`, `title`, `date`, `creators`, `tags`, `collections`, `notes` (key and title)
and `attachments` (key, title, contentType, `path`, `exists`, number of `annotations`).
`exists: false` means the file is not on this computer (not downloaded): `text` cannot
read it.

## Changing

Commands that change something print the keys they changed (one per line), so they can be
chained; errors go to standard error, with exit code 1.

```sh
ournotes-cli set ABCD2345 title="A better title"            # FIELD=VALUE..., empty VALUE removes
ournotes-cli set ABCD2345 date=2024-05-01 extra="Read for week 3"
ournotes-cli tag ABCD2345 EFGH6789 --add "exam,week-3" --remove "todo"

ournotes-cli add-note ABCD2345 - < summary.md               # a note under the item, Markdown on stdin
ournotes-cli add-note --collection "FA26/CS8803" "Exam on Friday"  # a note of its own, in the collection
ournotes-cli edit-note NOTE2345 - < new.md                  # replace its text
ournotes-cli edit-note NOTE2345 --append --text "Checked again on Friday."

ournotes-cli new-collection "Week 1" --parent "FA26/CS8803"        # prints the new key
ournotes-cli file ABCD2345 --collection "FA26/CS8803/Week 1"        # unfile takes it out
ournotes-cli trash ABCD2345                                 # restore brings it back

ournotes-cli add 10.1038/nature14539 --collection "FA26/CS8803"     # a DOI, arXiv ID, ISBN, PMID
ournotes-cli add paper.pdf --collection "FA26/CS8803"               # a PDF; its details are looked up
ournotes-cli import refs.bib --collection "FA26/CS8803"             # BibTeX, RIS, CSL JSON
```

- `set` checks the field against the item's type and lists the fields it has when one is
  wrong. Authors (`creators`) are not changed with `set`; neither are notes (use
  `edit-note`) or annotations.
- Text for a note: an argument, or standard input with `-` (best for anything longer than a
  line, and for text that begins with `-`). It is read as Markdown unless `--html` or
  `--text` says otherwise, or it begins with `<` (then it is taken as HTML).
- Markdown in notes: paragraphs, `#` headings (`###` and deeper become level 3), `-` and
  `1.` lists (not nested), `>` quotes, code blocks between ```` ``` ````, `---`,
  `**bold**`, `*italic*`, `` `code` ``, `[links](https://...)`, and formulas: `$x^2$` in a
  line, `$$...$$` on lines of their own, shown as formulas in the app.
- Nothing is deleted for good: `trash` moves items to the trash, where the user can look
  them over. Do not empty the trash.

## Recipes

**A summary of each paper in a collection, as a note under it**

```sh
ournotes-cli items --collection "FA26/CS8803" --json > items.json
# for each item in items.json whose attachments include a PDF that exists:
ournotes-cli text "$KEY" --pages 20 > paper.txt          # and/or: ournotes-cli annotations "$KEY"
# ... write summary.md from paper.txt ...
ournotes-cli add-note "$KEY" - < summary.md
```

Start a summary note with a heading (e.g. `# Summary`): the first line is the note's title
in the app.

**One overview of the whole collection**: read the items as above, then
`ournotes-cli add-note --collection "FA26/CS8803" - < overview.md`.

**Rename every title in a collection** (here: a course prefix)

```sh
ournotes-cli items --collection "FA26/CS8803" --json |
  python3 -c 'import json,sys; [print(i["key"] + "\t" + i["title"]) for i in json.load(sys.stdin)]' |
  while IFS=$'\t' read -r key title; do
    ournotes-cli set "$key" "title=CS8803: $title"
  done
```

## Sync, and the app running at the same time

- The app may be open while you work: it notices another program's changes within a couple
  of seconds and shows them. With *Sync automatically* on, it syncs them about 20 seconds
  later; `ournotes-cli sync` sends them at once (and brings in the others' changes).
- With the app closed, it syncs when it is next opened.
- If the same item was changed on another computer meanwhile, the later change wins and
  the other is kept under *Tools → Sync Conflicts*.

## Good manners

- List before you change: show the user what a bulk change will touch (`items --json`),
  and ask before renaming, retagging or trashing many items.
- Prefer adding (a note, a tag) to rewriting what the user wrote: `edit-note` replaces a
  note's text, so read it first (`text NOTE2345`), and use `--append` to add to it.
- Changes reach every computer and the server the library syncs with.
