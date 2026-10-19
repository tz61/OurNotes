#pragma once

#include <QString>
#include <QTextFormat>

#include <cstdint>

class QTextDocument;

namespace fp::gui {

// Character format properties used by the note editor.
constexpr int kMathSourceProperty = QTextFormat::UserProperty + 11;          // 1 inline / 2 display formula source
constexpr int kOriginalForegroundProperty = QTextFormat::UserProperty + 12;  // colour hidden for contrast

// Prepare stored note HTML for a QTextDocument: embedded images
// (<img data-attachment-key="KEY">) become "attachment:KEY" resources.
QString noteHtmlForEditor(const QString& html, int64_t libraryId, QTextDocument* doc);

// Serialise a QTextDocument to clean note HTML in Zotero's format
// (<div data-schema-version="9">…</div>), without Qt's inline styling.
QString documentToNoteHtml(const QTextDocument* doc);

} // namespace fp::gui
