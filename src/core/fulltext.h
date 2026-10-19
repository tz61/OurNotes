#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/sqlite.h"

namespace on {

// Full-text index of attachment contents (FTS5), kept in its own database file so
// background indexing never contends with edits in the main library database.
// Text extraction happens elsewhere (PDF module); this class only stores and queries.
class FulltextIndex {
public:
    explicit FulltextIndex(const fs::path& dbPath);

    bool isIndexed(int64_t lib, const std::string& key, const std::string& md5);
    void store(int64_t lib, const std::string& key, const std::string& md5, const std::string& text, int pages);
    void remove(int64_t lib, const std::string& key);
    void removeLibrary(int64_t lib);
    void clear();
    std::string text(int64_t lib, const std::string& key);
    int count(int64_t lib);

    // Attachment keys whose text matches all tokens (as produced by on::searchTokens).
    std::set<std::string> search(int64_t lib, const std::vector<std::string>& tokens);

private:
    sql::Db db_;
};

} // namespace on
