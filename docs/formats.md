# Import/export formats, metadata lookup and citations

The `ournotes_formats` library (`src/formats/`) converts items in Zotero Web API v3
"data" form (`on::Json`) to and from bibliographic exchange formats, fetches metadata
for identifiers, and formats citations. Everything is synchronous; network calls are
meant to run on worker threads.

## Item conventions

* Exporters take items in Zotero data form. Items may carry `_notes` (HTML strings of
  child notes) and `_attachments` (`{"title","path","contentType","url"}`); notes,
  attachments and annotations themselves are skipped.
* Importers return new items without `key`, with only fields valid for the item type
  (base fields are mapped, e.g. BibTeX `booktitle` becomes `proceedingsTitle` or
  `bookTitle`), plus optional `_notes` (HTML) and `_files` (local paths of attached files).

## Formats (`formats/formats.h`)

`allFormats()` lists the registered formats, `findFormat(id)` returns one, and
`detectFormat(text)` guesses the format of pasted or dropped data.

| id        | Extension | Import | Export | Notes |
|-----------|-----------|--------|--------|-------|
| `bibtex`  | `.bib`    | yes    | yes    | BibTeX and the common BibLaTeX fields |
| `ris`     | `.ris`    | yes    | yes    | Zotero/EndNote flavour, CRLF output |
| `csljson` | `.json`   | yes    | yes    | CSL-JSON using the schema's CSL mappings |

### BibTeX (`formats/bibtex.h`)

* **Export:** citation keys are `lastnameYearFirstword` (for example `lecun2015deep`),
  ASCII-folded and made unique with `a`, `b`, `c` suffixes. A `citationKey` field or a
  `Citation Key: …` line in Extra takes precedence. LaTeX specials (`{}&%$#_\~^`) are
  escaped and Unicode is kept. Acronyms and words with internal capitals are wrapped in
  braces in titles so their case survives. Type mapping: journal article → `@article`,
  conference paper → `@inproceedings`, book section → `@incollection`, thesis →
  `@phdthesis`/`@mastersthesis` (based on the thesis type), report → `@techreport`,
  software → `@software`, manuscript → `@unpublished`, anything else → `@misc`. arXiv
  preprints get `eprint`, `archivePrefix` and `primaryClass` (taken from an
  `arXiv:ID [class]` line). Web pages get `howpublished = {\url{…}}`. Extra is written
  as `note`, child notes as `annote`, and attachments with a local path as a
  JabRef/Zotero `file` field.
* **Import:** a hand-written, linear-time parser. It handles nested braces, quoted
  values, `#` concatenation, `@string` and month macros, and ignores `@comment` and
  `@preamble`. It recovers from broken entries at the next line that starts a new
  entry. LaTeX is decoded to Unicode, including accents (`{\"o}`, `\'{e}`, `\v{c}`),
  special letters (`\ss`, `\o`, `\aa`), escapes, dashes, quotes, Greek letters and
  formatting commands. Name lists support "Last, First", "First von Last",
  "Last, Jr., First" and `{Organisation Names}` (which become single-field creators).
  arXiv entries are recognised from `eprint`/`archivePrefix`, Google Scholar's
  `journal = {arXiv preprint arXiv:…}` and DBLP's `CoRR` volumes. They become preprints
  with `archiveID` and the arXiv DOI. `keywords` become tags, and `file` fields become
  `_files`. Relative paths are returned as written, so resolve them against the `.bib`
  file's directory.

### RIS (`formats/ris.h`)

Supported types include `JOUR`, `CONF`/`CPAPER`, `BOOK`, `CHAP`, `THES`, `RPRT`, `ELEC`,
`COMP`, `GEN` and `UNPB`, plus most other RIS types. RIS has no preprint type, so
preprints are exported as `UNPB`. On import, `UNPB` becomes a preprint when it looks
like one (arXiv/bioRxiv/medRxiv/SSRN, an `arXiv:` number or an arXiv DOI) and a
manuscript otherwise. Supported tags:

* Creators: `AU`/`A1` (authors), `A2`/`ED` (editors), `A3` (series editors),
  `A4` (translators). A name without a comma is imported as an organisation.
* Titles: `TI`/`T1`, `T2`/`JO`/`JF`/`BT` (container), `J2`/`JA` (abbreviation),
  `T3` (series).
* Dates: `PY`/`Y1`/`DA`, keeping the most precise; `Y2` is the access date.
* Other fields: `VL`, `IS`, `SP`/`EP`, `DO`, `UR`, `AB`/`N2`, `KW`, `SN` (ISBN or ISSN),
  `PB`, `CY`, `LA`, `ET`, `M1`, `M3`, `DP`, `ST`, `CN`.
* Notes and files: `N1` is imported as a note. `L1`/`L4` (plain paths or `file://`
  URLs) become `_files`.

Continuation lines are joined, and a UTF-8 BOM and CRLF line endings are accepted.

### CSL-JSON (`formats/csljson.h`)

`toCsl`/`fromCsl` use the `csl` section of `resources/schema.json` for types, fields and
creator roles. Dates are written as `{"date-parts": [[y, m, d]]}`. Accepted input dates
include `2023-05-01`, `2023-05`, `May 2023`, `12 May 2023`, `May 12, 2023`, `2023`,
`5/12/2023` and `2015 May 28`; anything else becomes `{"raw": …}`. The item key is
used as `id`. Tags are written to `keyword`.

## Identifier lookup (`formats/metadata.h`)

* `parseIdentifiers(text)` reads identifiers typed or pasted by the user, separated by
  whitespace, commas or newlines:
  * DOIs, bare, with a `doi:` prefix or as a doi.org URL.
  * arXiv IDs: new style `2301.12345v2`, old style `hep-th/9901001`, with an `arXiv:`
    prefix or as an `arxiv.org/abs|pdf` URL.
  * ISBN-10/13, with the checksum validated.
  * PMIDs, only with an explicit `PMID:` prefix.
* `findIdentifiersInText(text)` scans running text such as a PDF's first page. It finds
  DOIs (with trailing punctuation trimmed), arXiv stamps
  (`arXiv:2301.08243v3 [cs.CV] 25 Jan 2023`), `ISBN …` and `PMID: …`.
* `lookup(id)` returns an item shaped like Zotero's translators produce:

| Kind  | Source | Item |
|-------|--------|------|
| DOI   | `api.crossref.org/works/{doi}`; non-Crossref DOIs (DataCite, mEDRA …) via doi.org content negotiation | type from the Crossref type, Library Catalog "DOI.org (Crossref)" |
| DOI `10.48550/arXiv.*`, arXiv | `export.arxiv.org/api/query` (Atom) | preprint with repository "arXiv", archive ID `arXiv:ID`, the arXiv DOI and Extra `arXiv:ID [primary class]`; the comment becomes a child note, and the journal reference and published DOI go to Extra |
| ISBN  | Open Library books API | book |
| PMID  | NCBI E-utilities `efetch` | journal article with abstract, PMID and PMCID |

* `lookupByTitle(title, authorHint = "")` searches Crossref (`query.bibliographic`, top
  3 hits). It accepts a hit only when the normalised titles are at least 90% similar
  (Levenshtein ratio). For short generic titles, pass the first author's last name.
* `downloadFile(url, dest, requirePdf)` streams to `dest.part` and renames it on
  success. With `requirePdf` it rejects anything that does not start with `%PDF`.
  `arxivPdfUrl(id)` builds the arXiv PDF URL.

All requests use HTTPS, 15 s connect/read timeouts and the User-Agent
`OurNotes/<version> (mailto:ournotes@users.noreply.github.com)`. Redirects are followed
manually across hosts, up to 5. A dropped connection or a 429/503 answer is retried
once. Failures throw `MetadataError` with a message that can be shown to the user (not
found, network error, timeout, HTTPS unavailable).

## Citation styles (`formats/citation.h`)

The styles are implemented directly in C++; there is no CSL engine yet:

| id | Style | Kind |
|----|-------|------|
| `apa` | APA 7th edition | author-date |
| `chicago-author-date` | Chicago Manual of Style 17th edition (author-date) | author-date |
| `ieee` | IEEE | numeric `[1]` |
| `mla` | MLA 9th edition | author (no date in citations) |
| `harvard` | Harvard, Cite Them Right 12th edition | author-date |
| `nature` | Nature | numeric, superscript |
| `vancouver` | Vancouver (NLM) | numeric `(1)` |

`formatBibliography(items, style, Text|Html)` handles:

* Ordering: author-date styles sort by first author, year and title; numeric styles
  keep the given order and number the entries.
* Output: HTML wraps each entry in `<div class="csl-entry">` and italicises
  container and book titles.
* Name rules: each style's "et al." rule, for example APA's 20 authors and the
  "…" rule. Organisations are supported, and editors stand in for missing authors.
* Item types: journal articles, books, chapters, conference papers, theses, reports,
  web pages (with access dates where the style wants them) and arXiv preprints,
  formatted as Zotero formats them.
* Formatting details: page ranges use the style's en-dash and abbreviation rules
  (Chicago, MLA, Vancouver). Missing dates print as "n.d." ("no date" in Cite Them
  Right). DOIs print as `https://doi.org/…`.

`formatCitation(items, style)` produces the in-text citation, for example
`(Smith & Jones, 2020; Lee et al., 2021)` in APA or `[1], [2]` in IEEE.

Known gaps: there is no year-suffix disambiguation (2020a/2020b), no title-case or
sentence-case conversion (titles print as stored), and no locators (page numbers) in
in-text citations.
