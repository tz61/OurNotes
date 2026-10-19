#include "core/fulltext.h"

namespace fp {

FulltextIndex::FulltextIndex(const fs::path& dbPath)
{
    fs::create_directories(dbPath.parent_path());
    db_.open(dbPath);
    db_.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    db_.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS docs(
            id INTEGER PRIMARY KEY,
            library_id INTEGER NOT NULL,
            key TEXT NOT NULL,
            md5 TEXT NOT NULL DEFAULT '',
            pages INTEGER NOT NULL DEFAULT 0,
            indexed_at TEXT NOT NULL DEFAULT '',
            UNIQUE(library_id, key));
        CREATE VIRTUAL TABLE IF NOT EXISTS content USING fts5(
            text, tokenize = 'unicode61 remove_diacritics 2');
    )SQL");
}

bool FulltextIndex::isIndexed(int64_t lib, const std::string& key, const std::string& md5)
{
    auto stored = db_.scalarStr("SELECT md5 FROM docs WHERE library_id=? AND key=?", lib, key);
    return stored && *stored == md5;
}

void FulltextIndex::store(int64_t lib, const std::string& key, const std::string& md5, const std::string& text,
                          int pages)
{
    sql::Transaction tx(db_);
    remove(lib, key);
    db_.run("INSERT INTO docs(library_id, key, md5, pages, indexed_at) VALUES(?,?,?,?,?)", lib, key, md5, pages,
            nowIso());
    db_.run("INSERT INTO content(rowid, text) VALUES(?,?)", db_.lastInsertId(), text);
    tx.commit();
}

void FulltextIndex::remove(int64_t lib, const std::string& key)
{
    sql::Transaction tx(db_);
    if (auto id = db_.scalarInt("SELECT id FROM docs WHERE library_id=? AND key=?", lib, key)) {
        db_.run("DELETE FROM content WHERE rowid=?", *id);
        db_.run("DELETE FROM docs WHERE id=?", *id);
    }
    tx.commit();
}

void FulltextIndex::removeLibrary(int64_t lib)
{
    sql::Transaction tx(db_);
    db_.run("DELETE FROM content WHERE rowid IN (SELECT id FROM docs WHERE library_id=?)", lib);
    db_.run("DELETE FROM docs WHERE library_id=?", lib);
    tx.commit();
}

void FulltextIndex::clear()
{
    db_.exec("DELETE FROM content; DELETE FROM docs;");
}

std::string FulltextIndex::text(int64_t lib, const std::string& key)
{
    return db_
        .scalarStr("SELECT c.text FROM content c JOIN docs d ON d.id=c.rowid WHERE d.library_id=? AND d.key=?", lib,
                   key)
        .value_or("");
}

int FulltextIndex::count(int64_t lib)
{
    return (int)db_.scalarInt("SELECT COUNT(*) FROM docs WHERE library_id=?", lib).value_or(0);
}

std::set<std::string> FulltextIndex::search(int64_t lib, const std::vector<std::string>& tokens)
{
    std::set<std::string> out;
    std::string match;
    for (auto& t : tokens) {
        std::string tok = replaceAll(t, "\"", "\"\"");
        if (trim(tok).empty())
            continue;
        if (!match.empty())
            match += " AND ";
        // Phrases must match exactly; single words match as prefixes.
        match += "\"" + tok + "\"" + (tok.find(' ') == std::string::npos ? "*" : "");
    }
    if (match.empty())
        return out;
    try {
        auto st = db_.query(
            "SELECT d.key FROM content c JOIN docs d ON d.id=c.rowid WHERE content MATCH ? AND d.library_id=?",
            match, lib);
        while (st.step())
            out.insert(st.str(0));
    } catch (const sql::Error&) {
        // malformed FTS query: treat as no match
    }
    return out;
}

} // namespace fp
