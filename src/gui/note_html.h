#pragma once

#include <QString>

#include <cstdint>

class QTextDocument;

namespace fp::gui {

// Prepare stored note HTML for a QTextDocument: embedded images
// (<img data-attachment-key="KEY">) become "attachment:KEY" resources.
QString noteHtmlForEditor(const QString& html, int64_t libraryId, QTextDocument* doc);

// Serialise a QTextDocument to clean note HTML in Zotero's format
// (<div data-schema-version="9">…</div>), without Qt's inline styling.
QString documentToNoteHtml(const QTextDocument* doc);

} // namespace fp::gui
