#pragma once

#include <QString>
#include <QTextFormat>

#include <cstdint>

class QTextDocument;

namespace on::gui {

// Character format properties used by the note editor.
constexpr int kMathSourceProperty = QTextFormat::UserProperty + 11;          // 1 inline / 2 display formula source
constexpr int kOriginalForegroundProperty = QTextFormat::UserProperty + 12;  // colour hidden for contrast
constexpr int kHeadingMarkersProperty = QTextFormat::UserProperty + 13;     // block: a heading shown as "## …"

// Prepare stored note HTML for a QTextDocument: embedded images
// (<img data-attachment-key="KEY">) become "attachment:KEY" resources.
QString noteHtmlForEditor(const QString& html, int64_t libraryId, QTextDocument* doc);

// Serialise a QTextDocument to clean note HTML in Zotero's format
// (<div data-schema-version="9">…</div>), without Qt's inline styling.
QString documentToNoteHtml(const QTextDocument* doc);

// Annotation comments are plain text in which $…$ and $$…$$ are formulas (as Zotero
// keeps them). As HTML for a note editor, with the formulas rendered:
QString commentHtmlForEditor(const QString& comment);
// and back from the editor's document.
QString documentToComment(const QTextDocument* doc);

} // namespace on::gui
