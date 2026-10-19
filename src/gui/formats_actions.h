#pragma once

#include <QStringList>

#include <cstdint>
#include <string>

class QWidget;

namespace fp::gui {

struct Scope;

// Import/export, citations and metadata lookup (built on the formats module).
void exportItemsDialog(QWidget* parent, const Scope& scope, const QStringList& keys);
void importFileDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey);
void bibliographyDialog(QWidget* parent, const Scope& scope, const QStringList& keys);
void copyCitationToClipboard(QWidget* parent, int64_t libraryId, const QStringList& keys, bool bibliography);
void addByIdentifierDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey);
// Find metadata for standalone PDF attachments and create parent items for them.
void retrieveMetadata(QWidget* parent, int64_t libraryId, const QStringList& attachmentKeys);

} // namespace fp::gui
