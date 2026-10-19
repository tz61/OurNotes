#pragma once

#include <QStringList>

#include <cstdint>
#include <string>

#include "core/json.h"

class QWidget;

namespace fp::gui {

struct Scope;
class MetadataProgressDialog;

// Import/export, citations and metadata lookup (built on the formats module).
void exportItemsDialog(QWidget* parent, const Scope& scope, const QStringList& keys);
void importFileDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey);
void bibliographyDialog(QWidget* parent, const Scope& scope, const QStringList& keys);
void copyCitationToClipboard(QWidget* parent, int64_t libraryId, const QStringList& keys, bool bibliography);
void addByIdentifierDialog(QWidget* parent, int64_t libraryId, const std::string& collectionKey);
// Find metadata for standalone PDF attachments and create parent items for them,
// showing progress in a window. Returns nullptr when there is nothing to look up.
MetadataProgressDialog* retrieveMetadata(QWidget* parent, int64_t libraryId, const QStringList& attachmentKeys);
// Make `item` (found metadata) the parent of a standalone attachment; returns the new
// parent's key, or an empty string if the attachment already has a parent.
QString applyMetadataMatch(int64_t libraryId, const QString& attachmentKey, const Json& item);

} // namespace fp::gui
