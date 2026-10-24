#pragma once

#include <QString>
#include <QTextFormat>

#include <cstdint>

class QTextBlock;
class QTextCharFormat;
class QTextDocument;
class QTextTable;

namespace on::gui {

// Character format properties used by the note editor.
constexpr int kMathSourceProperty = QTextFormat::UserProperty + 11;          // 1 inline / 2 display formula source
constexpr int kOriginalForegroundProperty = QTextFormat::UserProperty + 12;  // colour hidden for contrast
constexpr int kHeadingMarkersProperty = QTextFormat::UserProperty + 13;     // block: a heading shown as "## …"
// A picture's caption: the paragraph under a line of pictures (block), and its text (char:
// shown smaller and gray, which is not saved). Saved as <p class="caption">; Zotero shows
// it as a paragraph under the pictures.
constexpr int kCaptionProperty = QTextFormat::UserProperty + 14;

// Prepare stored note HTML for a QTextDocument: embedded images
// (<img data-attachment-key="KEY">) become "attachment:KEY" resources.
QString noteHtmlForEditor(const QString& html, int64_t libraryId, QTextDocument* doc);
// Zotero's formulas (<span class="math">$…$</span>, <pre class="math">$$…$$</pre>) as
// "math:" images, which the note editor renders: for notes, and for HTML pasted in.
QString mathMarkupToImages(const QString& html);
// A table's first row of <th> cells in a <thead>, which QTextDocument takes for a header
// row (QTextTableFormat::headerRowCount); written back as <th> (documentToNoteHtml).
QString tableHeadersForEditor(const QString& html);
// How tables look in a note: lines around the cells, some room in them, their contents at
// the top of their rows. For a table just made, or one with new rows or columns.
void styleNoteTable(QTextTable* table);
void styleNoteTables(QTextDocument* doc);
// A caption's text: smaller and gray (kCaptionProperty).
QTextCharFormat captionCharFormat();
// The block made a caption, or a paragraph again (its text as the paragraph's); and the
// captions the HTML had (see noteHtmlForEditor) made so, once the document has it.
void setCaption(QTextBlock block, bool caption);
void markCaptions(QTextDocument* doc);
// A copy of a note's text, made ready for Qt's HTML (which keeps none of our properties):
// captions marked as markCaptions finds them, their look (and gray) not left on the text,
// colours hidden for contrast (dark mode) as they are.
void markForCopy(QTextDocument* doc);
// Such a copy, read back (into a note): formulas' colours and captions as they were.
void readCopy(QTextDocument* doc);

// Serialise a QTextDocument to clean note HTML in Zotero's format
// (<div data-schema-version="9">…</div>), without Qt's inline styling.
QString documentToNoteHtml(const QTextDocument* doc);

// Annotation comments are plain text in which $…$ and $$…$$ are formulas (as Zotero
// keeps them). As HTML for a note editor, with the formulas rendered:
QString commentHtmlForEditor(const QString& comment);
// and back from the editor's document.
QString documentToComment(const QTextDocument* doc);

} // namespace on::gui
