#pragma once

#include <QString>
#include <QStringList>
#include <QTextDocument>

#include <string>
#include <vector>

#include "core/json.h"
#include "core/util.h"

namespace on::gui {

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

// A tooltip or message with text from a library (a title, a name), shown as it is: one
// that looks like HTML is rich text, and rich text reads the files its <img> tags name
// (one naming /dev/zero, synced from a group, took the app down).
inline QString asPlainText(const QString& s)
{
    return Qt::mightBeRichText(s) ? "<p style=\"white-space:pre-wrap\">" + s.toHtmlEscaped() + "</p>" : s;
}

// MIME type used when dragging items between views (JSON: {"library": id, "keys": [...]}).
inline const char* kItemsMime = "application/x-ournotes-items";

} // namespace on::gui
