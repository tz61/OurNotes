#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/util.h"

struct sqlite3;
struct sqlite3_stmt;

namespace fp::sql {

class Error : public fp::Error {
public:
    Error(const std::string& msg, int code) : fp::Error(msg), code(code) {}
    int code;
};

struct Null {};

class Stmt {
public:
    Stmt(sqlite3* db, std::string_view sql);
    ~Stmt();
    Stmt(Stmt&& o) noexcept;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
    Stmt& bind(int idx, T v)
    {
        return bindInt64(idx, (int64_t)v);
    }
    Stmt& bind(int idx, double v);
    Stmt& bind(int idx, std::string_view v);
    Stmt& bind(int idx, const std::string& v) { return bind(idx, std::string_view(v)); }
    Stmt& bind(int idx, const char* v) { return v ? bind(idx, std::string_view(v)) : bind(idx, Null{}); }
    Stmt& bind(int idx, Null);
    template <typename T>
    Stmt& bind(int idx, const std::optional<T>& v)
    {
        return v ? bind(idx, *v) : bind(idx, Null{});
    }

    template <typename... Args>
    Stmt& bindAll(const Args&... args)
    {
        int i = 1;
        (bind(i++, args), ...);
        return *this;
    }

    bool step();  // true while a row is available
    void exec();  // step until done
    void reset();

    int columnCount() const;
    bool isNull(int col) const;
    int64_t i64(int col) const;
    int i32(int col) const { return (int)i64(col); }
    double f64(int col) const;
    std::string str(int col) const;
    std::string blob(int col) const;

private:
    Stmt& bindInt64(int idx, int64_t v);

    sqlite3* db_;
    sqlite3_stmt* st_ = nullptr;
};

class Db {
public:
    Db() = default;
    explicit Db(const fs::path& path, bool readOnly = false);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void open(const fs::path& path, bool readOnly = false);
    void close();
    bool isOpen() const { return db_ != nullptr; }
    sqlite3* handle() const { return db_; }

    void exec(std::string_view sql);  // may contain several statements
    Stmt prepare(std::string_view sql) { return Stmt(db_, sql); }

    template <typename... Args>
    Stmt query(std::string_view sql, const Args&... args)
    {
        Stmt st(db_, sql);
        st.bindAll(args...);
        return st;
    }
    template <typename... Args>
    void run(std::string_view sql, const Args&... args)
    {
        query(sql, args...).exec();
    }
    template <typename... Args>
    std::optional<int64_t> scalarInt(std::string_view sql, const Args&... args)
    {
        auto st = query(sql, args...);
        if (st.step() && !st.isNull(0))
            return st.i64(0);
        return std::nullopt;
    }
    template <typename... Args>
    std::optional<std::string> scalarStr(std::string_view sql, const Args&... args)
    {
        auto st = query(sql, args...);
        if (st.step() && !st.isNull(0))
            return st.str(0);
        return std::nullopt;
    }

    int64_t lastInsertId() const;
    int changes() const;
    int userVersion();
    void setUserVersion(int v);

private:
    sqlite3* db_ = nullptr;
};

// Nestable transaction. The outermost level is BEGIN IMMEDIATE (takes the write
// lock up front, so concurrent connections in WAL mode never hit a stale-snapshot
// SQLITE_BUSY); inner levels are SAVEPOINTs. Rolls back unless commit() is called.
class Transaction {
public:
    explicit Transaction(Db& db);
    ~Transaction();
    void commit();

private:
    Db& db_;
    std::string name_;  // empty for the outermost level
    bool done_ = false;
};

} // namespace fp::sql
