#include "core/sync/protocol.h"

namespace fp::sync {

bool isObjectType(const std::string& t)
{
    return t == kCollection || t == kItem || t == kSetting || t == kSearch;
}

Json toJson(const RemoteObject& o)
{
    Json j{{"type", o.type}, {"key", o.key}, {"version", o.version}};
    if (o.deleted)
        j["deleted"] = true;
    else
        j["data"] = o.data;
    return j;
}

Json toJson(const RemoteLibrary& l)
{
    return Json{{"uid", l.uid},   {"name", l.name},   {"type", l.type},
                {"role", l.role}, {"owner", l.owner}, {"version", l.version}};
}

Json toJson(const ChangesPage& p)
{
    Json objs = Json::array();
    for (auto& o : p.objects)
        objs.push_back(toJson(o));
    Json j{{"version", p.version}, {"objects", objs}};
    j["next"] = p.next.empty() ? Json() : Json(p.next);
    return j;
}

Json toJson(const PushObject& o)
{
    Json j{{"type", o.type}, {"key", o.key}, {"baseVersion", o.baseVersion}};
    if (o.deleted)
        j["deleted"] = true;
    else
        j["data"] = o.data;
    return j;
}

Json toJson(const PushEntry& e)
{
    Json j{{"type", e.type}, {"key", e.key}, {"status", e.status}, {"version", e.version}};
    if (e.current)
        j["current"] = toJson(*e.current);
    if (!e.message.empty())
        j["message"] = e.message;
    return j;
}

Json toJson(const PushResult& r)
{
    Json res = Json::array();
    for (auto& e : r.results)
        res.push_back(toJson(e));
    return Json{{"version", r.version}, {"results", res}};
}

Json toJson(const FileInfo& f)
{
    return Json{{"key", f.key},   {"md5", f.md5},     {"filename", f.filename}, {"contentType", f.contentType},
                {"size", f.size}, {"mtime", f.mtime}};
}

RemoteObject remoteObjectFromJson(const Json& j)
{
    RemoteObject o;
    o.type = jstr(j, "type");
    o.key = jstr(j, "key");
    o.version = jint(j, "version");
    o.deleted = jbool(j, "deleted");
    if (!o.deleted && j.contains("data"))
        o.data = j["data"];
    return o;
}

RemoteLibrary remoteLibraryFromJson(const Json& j)
{
    RemoteLibrary l;
    l.uid = jstr(j, "uid");
    l.name = jstr(j, "name");
    l.type = jstr(j, "type", "user");
    l.role = jstr(j, "role", "owner");
    l.owner = jstr(j, "owner");
    l.version = jint(j, "version");
    return l;
}

ChangesPage changesPageFromJson(const Json& j)
{
    ChangesPage p;
    p.version = jint(j, "version");
    for (auto& o : j.value("objects", Json::array()))
        p.objects.push_back(remoteObjectFromJson(o));
    p.next = jstr(j, "next");
    return p;
}

PushObject pushObjectFromJson(const Json& j)
{
    PushObject o;
    o.type = jstr(j, "type");
    o.key = jstr(j, "key");
    o.baseVersion = jint(j, "baseVersion");
    o.deleted = jbool(j, "deleted");
    if (!o.deleted && j.contains("data"))
        o.data = j["data"];
    return o;
}

PushEntry pushEntryFromJson(const Json& j)
{
    PushEntry e;
    e.type = jstr(j, "type");
    e.key = jstr(j, "key");
    e.status = jstr(j, "status");
    e.version = jint(j, "version");
    if (j.contains("current") && j["current"].is_object())
        e.current = remoteObjectFromJson(j["current"]);
    e.message = jstr(j, "message");
    return e;
}

PushResult pushResultFromJson(const Json& j)
{
    PushResult r;
    r.version = jint(j, "version");
    for (auto& e : j.value("results", Json::array()))
        r.results.push_back(pushEntryFromJson(e));
    return r;
}

FileInfo fileInfoFromJson(const Json& j)
{
    FileInfo f;
    f.key = jstr(j, "key");
    f.md5 = jstr(j, "md5");
    f.filename = jstr(j, "filename");
    f.contentType = jstr(j, "contentType");
    f.size = jint(j, "size");
    f.mtime = jint(j, "mtime");
    return f;
}

} // namespace fp::sync
