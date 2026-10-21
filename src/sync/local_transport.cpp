#include "sync/local_transport.h"

#include <chrono>
#include <fstream>
#include <thread>

#include "core/hash.h"

namespace on::sync {

namespace {

// Removes a file when the scope is left by an exception (or without release()).
class FileGuard {
public:
    explicit FileGuard(fs::path path) : path_(std::move(path)) {}
    ~FileGuard()
    {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
    void release() { path_.clear(); }

private:
    fs::path path_;
};

// Copies src to dest, reporting progress; returns the md5 of the copied bytes.
std::string copyWithProgress(const fs::path& src, const fs::path& dest, const ProgressFn& progress)
{
    std::ifstream in(src, std::ios::binary);
    if (!in)
        throw SyncError("cannot read " + pathString(src));
    std::error_code ec;
    auto total = (int64_t)fs::file_size(src, ec);
    if (ec)
        throw SyncError("cannot read " + pathString(src) + ": " + ec.message());
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out)
        throw SyncError("cannot write " + pathString(dest));

    Md5 md5;
    std::vector<char> buf(1 << 20);
    int64_t done = 0;
    if (progress && !progress(0, total))
        throw SyncError("cancelled");
    for (;;) {
        in.read(buf.data(), (std::streamsize)buf.size());
        auto n = in.gcount();
        if (n <= 0)
            break;
        out.write(buf.data(), n);
        if (!out)
            throw SyncError("write failed: " + pathString(dest));
        md5.update(buf.data(), (size_t)n);
        done += n;
        if (progress && !progress(done, total))
            throw SyncError("cancelled");
    }
    if (in.bad())
        throw SyncError("read failed: " + pathString(src));
    out.close();
    if (!out)
        throw SyncError("write failed: " + pathString(dest));
    return md5.hexDigest();
}

} // namespace

LocalTransport::LocalTransport(const fs::path& folder) : folder_(folder), store_(folder, false)
{
    if (!store_.userExists(kUser)) {
        try {
            store_.createUser(kUser, randomToken(16));
        } catch (const SyncError& e) {
            if (e.status != 409)  // another process created it first
                throw;
        }
    }
}

Json LocalTransport::info()
{
    return Json{{"server", "local"}, {"path", pathString(folder_)}, {"api", kApiVersion}};
}

std::vector<RemoteLibrary> LocalTransport::libraries() { return store_.libraries(kUser); }

std::optional<LibrariesState> LocalTransport::awaitLibraries(const std::string& known, int wait)
{
    using namespace std::chrono;
    auto deadline = steady_clock::now() + seconds(wait);
    for (;;) {
        if (aborted_)
            throw SyncError("cancelled");
        LibrariesState now;
        now.libraries = store_.libraries(kUser);
        now.state = librariesState(now.libraries);
        if (known.empty() || now.state != known || steady_clock::now() >= deadline)
            return now;
        auto next = std::min(deadline, steady_clock::now() + seconds(kPollSeconds));
        while (!aborted_ && steady_clock::now() < next)
            std::this_thread::sleep_for(milliseconds(50));
    }
}

RemoteLibrary LocalTransport::ensureLibrary(const std::string& uid, const std::string& name, const std::string& type)
{
    return store_.ensureLibrary(uid, name, type, kUser);
}

void LocalTransport::deleteLibrary(const std::string& uid)
{
    if (requireLibrary(uid, false).role != "owner")
        throw SyncError("only the owner of library '" + uid + "' can delete it", 403);
    store_.deleteLibrary(uid, true);
}

RemoteLibrary LocalTransport::requireLibrary(const std::string& uid, bool write)
{
    auto lib = store_.library(uid, kUser);
    if (!lib)
        throw SyncError("library '" + uid + "' not found", 404);
    if (write && lib->role == "read")
        throw SyncError("read-only access to library '" + uid + "'", 403);
    return *lib;
}

ChangesPage LocalTransport::changes(const std::string& uid, int64_t since, const std::string& cursor, int limit)
{
    requireLibrary(uid, false);
    return store_.changes(uid, since, cursor, limit);
}

PushResult LocalTransport::push(const std::string& uid, const std::vector<PushObject>& objects)
{
    requireLibrary(uid, true);
    return store_.push(uid, objects);
}

std::vector<FileInfo> LocalTransport::files(const std::string& uid)
{
    requireLibrary(uid, false);
    return store_.files(uid);
}

FileInfo LocalTransport::download(const std::string& uid, const std::string& key, const fs::path& dest,
                                  const ProgressFn& progress)
{
    requireLibrary(uid, false);
    auto info = store_.file(uid, key);
    if (!info)
        throw SyncError("file '" + key + "' not found", 404);

    std::error_code ec;
    if (dest.has_parent_path())
        fs::create_directories(dest.parent_path(), ec);
    auto part = dest;
    part += ".part";
    FileGuard guard(part);
    auto md5 = copyWithProgress(store_.blobPath(uid, key), part, progress);
    if (md5 != info->md5)
        throw SyncError("checksum mismatch for file '" + key + "' (modified while copying?)");
    fs::rename(part, dest, ec);
    if (ec)
        throw SyncError("cannot write " + pathString(dest) + ": " + ec.message());
    guard.release();
    return *info;
}

FileInfo LocalTransport::upload(const std::string& uid, const std::string& key, const fs::path& src,
                                const FileInfo& meta, const ProgressFn& progress)
{
    requireLibrary(uid, true);
    auto tmp = store_.newTempPath();
    FileGuard guard(tmp);
    copyWithProgress(src, tmp, progress);
    FileInfo m = meta;
    m.key = key;
    if (m.filename.empty())
        m.filename = pathString(src.filename());
    auto info = store_.putFile(uid, key, tmp, m);
    guard.release();
    return info;
}

} // namespace on::sync
