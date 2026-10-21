#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/sync/protocol.h"

namespace on::sync {

// Waits on a sync target, on a thread of its own, for what other computers change there,
// and calls changed() when there is something to pull. A server tells at once (it holds
// a request until one of the libraries changes); a folder is looked at every few seconds.
//
// What this computer pushes itself is not reported: a change seen while it synced (or
// that it already pulled) is checked against the versions it has, once its sync is over.
// changed() is not called twice for the same state of the target (a sync that fails is
// not asked for again and again).
class ChangeWatcher {
public:
    struct Hooks {
        std::function<std::unique_ptr<Transport>()> connect;
        // The version of each library (by uid) last pulled from the target.
        std::function<std::map<std::string, int64_t>()> pulledVersions;
        // Syncs with the target started and finished, each counted: odd while one runs.
        std::function<uint64_t()> syncCount;
        std::function<void()> changed;
    };
    struct Timing {
        int waitSeconds = 50;  // a server's answer at the latest (below proxies' 60)
        int retrySeconds = 5;  // after an error, doubling up to maxRetrySeconds
        int maxRetrySeconds = 300;
        int unsupportedSeconds = 1800;  // asking an older server again
    };

    explicit ChangeWatcher(Hooks hooks) : ChangeWatcher(std::move(hooks), Timing()) {}
    ChangeWatcher(Hooks hooks, Timing timing);
    ~ChangeWatcher();  // stops, at once

    // The target cannot tell (a server before 0.1.10): nothing is watched for now.
    bool unsupported() const { return unsupported_; }

private:
    void run();
    void watch(Transport& transport, std::string& asked);
    uint64_t idle();  // waits for a running sync to end; its count then
    bool behind(const LibrariesState& remote);
    void pause(int seconds);
    void pauseShortly();  // a tenth of a second, to look at the sync count again
    bool begin(Transport* transport);  // false: stopping
    void end();

    Hooks hooks_;
    Timing timing_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> unsupported_{false};
    std::atomic<bool> finished_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    Transport* inFlight_ = nullptr;  // aborted by the destructor
    std::thread thread_;
};

} // namespace on::sync
