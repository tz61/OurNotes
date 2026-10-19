#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/library.h"

namespace fp {

enum class ZoteroFileMode {
    Copy,      // copy attachment files into freePen's storage (default)
    Hardlink,  // hard-link when on the same filesystem, else copy
    None       // metadata only
};

struct ZoteroImportOptions {
    fs::path zoteroDir;  // Zotero data directory (contains zotero.sqlite and storage/)
    ZoteroFileMode fileMode = ZoteroFileMode::Copy;
    bool includeTrash = true;
    bool importStyles = true;
    std::function<void(const std::string& stage, int done, int total)> progress;
    std::function<void(const std::string& message)> log;
    std::function<bool()> cancelled;
};

struct ZoteroImportReport {
    int libraries = 0;
    int collections = 0;
    int items = 0;
    int notes = 0;
    int attachments = 0;
    int annotations = 0;
    int updated = 0;
    int unchanged = 0;
    int filesCopied = 0;
    int filesMissing = 0;
    int64_t bytesCopied = 0;
    int styles = 0;
    std::vector<std::string> warnings;

    std::string summary() const;
};

// Best guess of the Zotero data directory: the profile's custom dataDir
// preference if set, otherwise ~/Zotero.
fs::path findZoteroDataDir();

// Import (or re-import) a Zotero data directory: libraries (personal and groups),
// collections, items with all fields/creators/tags/relations, notes, attachments
// (with files), annotations, tag colours, last-read page positions and CSL styles.
// Zotero's files are only read; its database is snapshotted to a temporary copy.
// Re-running updates items that changed in Zotero since the previous import.
ZoteroImportReport importZotero(Library& library, const ZoteroImportOptions& options);

} // namespace fp
