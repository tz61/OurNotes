# OurNotes Connector (browser extension)

The OurNotes Connector saves the page you are reading to OurNotes, with its PDF when
the page has one, much like Zotero's Connector. It is in `browser/chrome` and works in
Chrome and other Chromium-based browsers.

## Installing

1. Get the extension: the `OurNotes-Connector-<version>-chrome.zip` of a release
   (unzip it into a folder you keep), or the `browser/chrome` folder of the source.
2. Open `chrome://extensions`, turn on *Developer mode* (top right) and choose
   *Load unpacked*, then pick that folder.
3. Pin it from the extensions menu (the puzzle piece) if you like a button for it.

OurNotes has to be running: the extension hands pages to it. Its browser connector is
on unless turned off in *Preferences → Advanced*.

## Saving a page

Click the OurNotes button, or press **Alt+Shift+O** (changeable at
`chrome://extensions/shortcuts`). The page goes into the library or collection selected
in OurNotes' library tab; the popup shows where, and lets you pick another collection
and add tags. *Done* (or Enter in the tags field) keeps your changes.

What is saved comes from what the page says about itself, in this order:

- **Scholarly pages** (journals, publishers, repositories) carry Google Scholar's
  `citation_*` tags: authors, journal, volume, issue, pages, date, DOI, keywords (as
  tags) and the PDF's address. The item gets the right type: journal article,
  conference paper, book section, thesis, report or book.
- **arXiv** pages become preprints with the arXiv id and DOI, and their PDF.
- Otherwise **JSON-LD** (schema.org: news articles, blog posts, books), **Dublin Core**
  and **Open Graph** tags; at the least, a web page with the title, address,
  description and language.

When a page names a DOI or arXiv id but little else, OurNotes looks it up (Crossref or
arXiv) for the rest.

**The PDF** is fetched by the browser, so pages you can read because you are signed in
(a library's subscription) work too, and saved as the item's attachment. If the browser
cannot fetch it, OurNotes downloads it itself.

**A PDF tab** (a PDF open in the browser) is saved as a PDF on its own, and OurNotes
looks up its details as when a PDF is dropped into the library.

## How it talks to OurNotes

OurNotes listens on `http://127.0.0.1:23120` (Zotero's connector uses 23119, so both
can run), on this computer only:

| Request | |
|---|---|
| `GET /connector/ping` | `{app, version, api}` |
| `POST /connector/save` | `{item}`, a Zotero-style item; answers `{key, libraryId, target, targets}` |
| `POST /connector/attachment?parent=&library=&url=&title=` | the PDF's bytes (`application/pdf`); without a parent, a PDF on its own |
| `POST /connector/attachmentUrl` | `{parent, library, url, title}`: OurNotes fetches the PDF |
| `POST /connector/update` | `{key, library, target, tags}`: another collection (`"<library>/<collection key>"` or `"<library>"`), tags |

Only browser extensions get in. A web page cannot use it to add things to your
library: requests carrying a web page's `Origin` are refused, as are requests for any
host name other than `127.0.0.1` or `localhost` (DNS rebinding) and POSTs without the
`X-OurNotes-Connector` header. Items are cut down to the fields of their type.

## Not yet

- Firefox (the extension is written for Chrome's Manifest V3).
- Snapshots of the page itself (Zotero saves an HTML copy).
- Several items from a search results page.
