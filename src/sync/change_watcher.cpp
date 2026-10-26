#include "sync/change_watcher.h"

#include <chrono>

namespace on::sync {

using namespace std::chrono;

ChangeWatcher::ChangeWatcher(Hooks hooks, Timing timing) : hooks_(std::move(hooks)), timing_(timing)
{
    thread_ = std::thread([this] {
        run();
        finished_ = true;
    });
}

ChangeWatcher::~ChangeWatcher()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    // An abort can come just before the request starts, and miss it: again until done.
    while (!finished_) {
        {
            std::lock_guard lock(mutex_);
            if (inFlight_)
                inFlight_->abort();
        }
        std::this_thread::sleep_for(milliseconds(20));
    }
    thread_.join();
}

void ChangeWatcher::run()
{
    int retry = 0;
    std::string asked;  // the target's state a sync was last asked for in
    while (!stopping_) {
        std::unique_ptr<Transport> transport;
        bool failed = false;
        try {
            transport = hooks_.connect();
            watch(*transport, asked, retry);
        } catch (const std::exception&) {
            failed = true;
        }
        end();  // before the transport goes
        transport.reset();
        if (failed) {
            retry = retry ? std::min(retry * 2, timing_.maxRetrySeconds) : timing_.retrySeconds;
            pause(retry);
        }
    }
}

// Until an error (or the end): as long as the target is reachable, with one transport.
void ChangeWatcher::watch(Transport& transport, std::string& asked, int& retry)
{
    while (!stopping_) {
        uint64_t synced = idle();
        if (stopping_ || !begin(&transport))
            return;
        auto now = transport.awaitLibraries({}, 0);
        end();
        retry = 0;  // reached: the next error waits as the first did, not as long as the last
        if (!now) {
            unsupported_ = true;
            pause(timing_.unsupportedSeconds);
            continue;
        }
        unsupported_ = false;
        // what came while this computer synced (or before it watched) and it has not pulled
        bool ask = now->state != asked && behind(*now);
        std::string known = now->state;
        while (!ask && !stopping_ && hooks_.syncCount() == synced) {
            auto started = steady_clock::now();
            if (!begin(&transport))
                return;
            auto next = transport.awaitLibraries(known, timing_.waitSeconds);
            end();
            if (!next || stopping_ || hooks_.syncCount() != synced)
                break;  // synced meanwhile: what changed may be its own push, seen to above
            if (next->state != known) {
                known = next->state;
                ask = true;
            } else if (steady_clock::now() - started < seconds(timing_.waitSeconds) / 2) {
                pause(30);  // a target that does not hold the request: not asked more often
            }
        }
        if (!ask)
            continue;
        asked = known;
        hooks_.changed();
        // the sync asked for starts (or does not: then on, as before)
        for (auto until = steady_clock::now() + seconds(10);
             !stopping_ && hooks_.syncCount() == synced && steady_clock::now() < until;)
            pauseShortly();
    }
}

uint64_t ChangeWatcher::idle()
{
    for (;;) {
        auto n = hooks_.syncCount();
        if (n % 2 == 0 || stopping_)
            return n;
        pauseShortly();
    }
}

void ChangeWatcher::pauseShortly()
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, milliseconds(100), [&] { return stopping_.load(); });
}

bool ChangeWatcher::behind(const LibrariesState& remote)
{
    auto pulled = hooks_.pulledVersions();
    for (auto& l : remote.libraries) {
        auto it = pulled.find(l.uid);
        if (it == pulled.end() || l.version > it->second)
            return true;
    }
    return false;
}

void ChangeWatcher::pause(int seconds)
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, std::chrono::seconds(seconds), [&] { return stopping_.load(); });
}

bool ChangeWatcher::begin(Transport* transport)
{
    std::lock_guard lock(mutex_);
    if (stopping_)
        return false;
    inFlight_ = transport;
    return true;
}

void ChangeWatcher::end()
{
    std::lock_guard lock(mutex_);
    inFlight_ = nullptr;
}

} // namespace on::sync
