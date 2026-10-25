# Working with an OurNotes library from an agent or a script

This is for AI agents (Claude Code, Codex, and the like) and scripts that read or change
someone's OurNotes library: summarise the papers in a collection, rename titles, write
notes, tag and file items, annotate PDFs. What the app does with a library,
`ournotes-cli` does too: `ournotes-cli help` lists every command. `ournotes-cli guide`
prints this page.

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
ournotes-cli items --tag exam --json       # also --unfiled (in no collection), --trash
ournotes-cli tags --json                   # tags, how many items have each, their colours
ournotes-cli types book --json             # a type's fields and kinds of authors
ournotes-cli related ABCD2345              # also: duplicates (items that look the same)
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

More, as in the app:

```sh
ournotes-cli new-item book title="Deep Learning" date=2016 --collection "FA26/CS8803"
ournotes-cli creators ABCD2345 "Goodfellow, Ian" "editor:Bengio, Yoshua" "OpenAI"  # replaces; --add
ournotes-cli set-type ABCD2345 bookSection                  # --force if it loses fields
ournotes-cli attach ABCD2345 slides.pdf                     # a file under the item; --link
ournotes-cli attach-url ABCD2345 https://arxiv.org/abs/1706.03762 --title "arXiv"
ournotes-cli move NOTE2345 --to EFGH6789                    # a note or attachment elsewhere
ournotes-cli related ABCD2345 --add EFGH6789                # --remove
ournotes-cli merge ABCD2345 EFGH6789                        # duplicates, into the oldest
ournotes-cli retrieve-metadata PDF23456                     # a PDF of its own gets its item
ournotes-cli rename-collection "FA26/CS8803" "CS 8803"      # move-collection C --parent P
ournotes-cli rename-tag todo later     # tag-color later red
```

- `set` checks the field against the item's type and lists the fields it has when one is
  wrong. Authors are changed with `creators` ("Last, First", or one name for an
  organisation; `editor:` and the like before it), the type with `set-type`, a note's
  text with `edit-note`.
- A tag is renamed, coloured or deleted in one library, as in the app: when several have
  it, say which with `--library`.
- Text for a note: an argument, or standard input with `-` (best for anything longer than a
  line, and for text that begins with `-`). It is read as Markdown unless `--html` or
  `--text` says otherwise, or it begins with `<` (then it is taken as HTML).
- Markdown in notes: paragraphs, `#` headings (`###` and deeper become level 3), `-` and
  `1.` lists (not nested), `>` quotes, code blocks between ```` ``` ````, `---`, tables
  (a header row, a `|---|:-:|--:|` row, then rows of cells; `\|` for a | in a cell),
  `**bold**`, `*italic*`, `` `code` ``, `[links](https://...)`, pictures
  (`![what it shows](figure.png)`: a PNG, JPEG, GIF or SVG file, a web address or a
  `data:` URI, stored in the note as the app stores a pasted one), and formulas: `$x^2$`
  in a line, `$$...$$` on lines of their own, shown as formulas in the app.
  `add-image NOTE a.png b.png [--row] [--caption TEXT] [--width PX | --height PX]` puts
  pictures at the end of a note, each on a line of its own, or side by side with `--row`
  (a row: the app shows them as high as each other, as wide as the note at most), with a
  caption under them. Without a size a picture is shown small (within 360 x 240), as a
  pasted one. In HTML, pictures alone in one `<p>` are a row and a `<p class="caption">`
  right under them is their caption. `text NOTE` says where each picture's file is
  (`[picture: PATH]`), to look at it.
- `trash` moves items to the trash, where the user can look them over. What deletes for
  good (`delete`, `empty-trash`, `delete-collection`, `delete-tag`, `delete-library`)
  asks for `--yes`: use it only when the user asks for exactly that.

## Annotations

On a PDF (its key, or the key of the item it is under); X,Y are points from the page's
top left:

```sh
ournotes-cli annotate ABCD2345 --quote "attention is all" --comment "key idea" --color yellow
ournotes-cli annotate ABCD2345 --quote "Theorem 2" --type underline --page 4
ournotes-cli annotate ABCD2345 --type note --quote "Theorem 2" --comment "check the proof"
ournotes-cli annotate ABCD2345 --type text --page 1 --at 72,700 --comment "Read for week 3"
ournotes-cli annotation ANNO2345 --comment "..." --tags "exam"   # change one; --color, --rect
ournotes-cli note-from-annotations ABCD2345        # a note quoting them, under the item
ournotes-cli export-pdf ABCD2345 -o annotated.pdf  # a copy of the PDF with them in it
```

To look at annotations as they are drawn, doodles (`ink`) above all, make a picture:

```sh
ournotes-cli render ABCD2345 --page 3 -o page3.png       # the page with its annotations
ournotes-cli render INK23456 -o doodle.png               # a doodle's place on its page
ournotes-cli render INK23456 --no-page -o doodle.png     # the doodle alone, on clear
```

It prints the picture's path; `--dpi` (150), `--rect X0,Y0,X1,Y1` for a part of a page,
`--margin` (18 points) around an annotation.

`--quote` finds the words (in any case) on `--page`, or on the first page that has them.
Areas (`--type image --rect X0,Y0,X1,Y1`) and drawings (`--type ink --ink "X,Y X,Y"`)
are made too; `view-state KEY --page N --zoom 150` sets where the app opens a PDF.

## Links to items, notes and pages

A note can link to an item, another note, a page of a PDF or an annotation; in the app a
click on such a link opens it (an item is shown in the library). `link` prints one:

```sh
ournotes-cli link ABCD2345                     # ournotes://item/<library>/ABCD2345
ournotes-cli link ABCD2345 --page 3 --markdown # [(Smith and Jones, 2020, p. 3)](ournotes://...): its PDF, page 3
ournotes-cli link ANNO2345 --markdown --quote "attention is all"  # “attention is all” [(...)](...)
ournotes-cli link NOTE2345 --markdown          # [the note's title](ournotes://...)
```

Put the Markdown into a note's text (`add-note`, `edit-note --append`) and it shows as the
app's own references do: “quote” (Smith and Jones, 2020, p. 3), the reference a link.
`--page` counts the PDF's pages from 1 (not by the numbers printed on them, which the words
use when the PDF has them), and on an item means its PDF. `--html` prints it as HTML,
`--json` all of these. A link names its library by an ID that is the same on every computer
the library syncs to. `note-from-annotations` links each quote to its annotation.

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

Leave syncing to the app: do not run `ournotes-cli sync` unless the user asks for it.

- The app may be open while you work: it notices another program's changes within a couple
  of seconds and shows them, and (with *Sync automatically* on, as it is unless the user
  turned it off) syncs them about 20 seconds later.
- With the app closed, it syncs them when it is next opened.
- When the user wants them on their other computers now (with the app closed, say),
  `ournotes-cli sync` sends them at once and brings in the others' changes.
- Either way the user can look over what you did in the app's *Tools → Sync History*: your
  changes are marked as made by `ournotes-cli`, and a sync you ran as synced by it.
- If the same item was changed on another computer meanwhile, the later change wins and
  the other is kept under *Tools → Sync Conflicts* (`ournotes-cli conflicts`).
- `sync-history` and `storage` show what the app shows under *Tools*. `config` shows and
  changes the app's preferences; the app reads them when it starts and writes its own back
  when it quits, so change them while it is closed.

## Good manners

- List before you change: show the user what a bulk change will touch (`items --json`),
  and ask before renaming, retagging or trashing many items.
- Prefer adding (a note, a tag) to rewriting what the user wrote: `edit-note` replaces a
  note's text, so read it first (`text NOTE2345`), and use `--append` to add to it.
- Changes reach every computer and the server the library syncs with.
