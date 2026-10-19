#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "core/json.h"
#include "core/util.h"

namespace fp::gui {

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline std::string ss(const QString& s) { return s.toStdString(); }
inline QString qpath(const fs::path& p) { return QString::fromStdString(pathString(p)); }
inline fs::path fspath(const QString& s) { return u8path(s.toStdString()); }
inline QString jqs(const Json& j, const char* key) { return qs(jstr(j, key)); }

inline QStringList toQStringList(const std::vector<std::string>& v)
{
    QStringList out;
    for (auto& s : v)
        out << qs(s);
    return out;
}

inline std::vector<std::string> toStdVector(const QStringList& v)
{
    std::vector<std::string> out;
    for (auto& s : v)
        out.push_back(ss(s));
    return out;
}

// MIME type used when dragging items between views (JSON: {"library": id, "keys": [...]}).
inline const char* kItemsMime = "application/x-freepen-items";

} // namespace fp::gui
