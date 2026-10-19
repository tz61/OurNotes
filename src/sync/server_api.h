#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sync/store.h"

namespace httplib {
class Server;
struct Response;
}

namespace on::sync {

struct ServerOptions {
    int64_t maxUploadBytes = 1LL << 30;
    std::string serverName = "ournotesd";
};

// The ournotesd HTTP API under /api/v1 (see docs/sync-protocol.md).
//
// mount() registers the routes plus a JSON error handler, a pre-routing
// request-size guard and the server's payload limit. The Store must outlive the
// httplib::Server; the ApiServer itself may be destroyed after mount().
class ApiServer {
public:
    explicit ApiServer(Store& store, ServerOptions options = {});
    ~ApiServer();

    void mount(httplib::Server& server);

    // The authenticated user of a handled request ("" if none), for access logs.
    static std::string requestUser(const httplib::Response& res);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace on::sync
