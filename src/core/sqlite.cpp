#include "core/sqlite.h"

#include <atomic>

#include <sqlite3.h>

namespace fp::sql {

static void check(sqlite3* db, int rc, const char* what)
{
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
        throw Error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)), rc);
}

Stmt::Stmt(sqlite3* db, std::string_view sql) : db_(db)
{
    if (!db)
        throw Error("database is not open", SQLITE_MISUSE);
    int rc = sqlite3_prepare_v2(db, sql.data(), (int)sql.size(), &st_, nullptr);
    if (rc != SQLITE_OK)
        throw Error(std::string(sqlite3_errmsg(db)) + " in: " + std::string(sql.substr(0, 200)), rc);
}

Stmt::~Stmt()
{
    if (st_)
        sqlite3_finalize(st_);
}

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), st_(o.st_) { o.st_ = nullptr; }

Stmt& Stmt::bind(int idx, int64_t v)
{
    check(db_, sqlite3_bind_int64(st_, idx, v), "bind");
    return *this;
}

Stmt& Stmt::bind(int idx, double v)
{
    check(db_, sqlite3_bind_double(st_, idx, v), "bind");
    return *this;
}

Stmt& Stmt::bind(int idx, std::string_view v)
{
    check(db_, sqlite3_bind_text(st_, idx, v.data(), (int)v.size(), SQLITE_TRANSIENT), "bind");
    return *this;
}

Stmt& Stmt::bind(int idx, Null)
{
    check(db_, sqlite3_bind_null(st_, idx), "bind");
    return *this;
}

bool Stmt::step()
{
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    throw Error(std::string("step: ") + sqlite3_errmsg(db_), rc);
}

void Stmt::exec()
{
    while (step()) {
    }
}

void Stmt::reset()
{
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}

int Stmt::columnCount() const { return sqlite3_column_count(st_); }
bool Stmt::isNull(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }
int64_t Stmt::i64(int col) const { return sqlite3_column_int64(st_, col); }
double Stmt::f64(int col) const { return sqlite3_column_double(st_, col); }

std::string Stmt::str(int col) const
{
    auto p = (const char*)sqlite3_column_text(st_, col);
    return p ? std::string(p, (size_t)sqlite3_column_bytes(st_, col)) : std::string();
}

std::string Stmt::blob(int col) const
{
    auto p = (const char*)sqlite3_column_blob(st_, col);
    return p ? std::string(p, (size_t)sqlite3_column_bytes(st_, col)) : std::string();
}

Db::Db(const fs::path& path, bool readOnly) { open(path, readOnly); }

Db::~Db() { close(); }

void Db::open(const fs::path& path, bool readOnly)
{
    close();
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(pathString(path).c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        close();
        throw Error("cannot open " + pathString(path) + ": " + msg, rc);
    }
    sqlite3_busy_timeout(db_, 15000);
    sqlite3_extended_result_codes(db_, 1);
}

void Db::close()
{
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void Db::exec(std::string_view sql)
{
    char* err = nullptr;
    std::string s(sql);
    int rc = sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw Error(msg, rc);
    }
}

int64_t Db::lastInsertId() const { return sqlite3_last_insert_rowid(db_); }
int Db::changes() const { return sqlite3_changes(db_); }
int Db::userVersion() { return (int)scalarInt("PRAGMA user_version").value_or(0); }
void Db::setUserVersion(int v) { exec("PRAGMA user_version=" + std::to_string(v)); }

static std::atomic<int> g_savepoint{0};

Transaction::Transaction(Db& db) : db_(db), name_("sp" + std::to_string(++g_savepoint))
{
    db_.exec("SAVEPOINT " + name_);
}

Transaction::~Transaction()
{
    if (!done_) {
        try {
            db_.exec("ROLLBACK TO " + name_);
            db_.exec("RELEASE " + name_);
        } catch (...) {
        }
    }
}

void Transaction::commit()
{
    db_.exec("RELEASE " + name_);
    done_ = true;
}

} // namespace fp::sql
