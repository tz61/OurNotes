#pragma once

#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "core/json.h"
#include "gui/items_model.h"

namespace httplib {
class Server;
}

namespace on::gui {

// The way in for the OurNotes Connector browser extension (browser/chrome): a small
// HTTP server on 127.0.0.1 that only browser extensions may use (their Origin is
// chrome-extension:// or moz-extension://, web pages' is not). A page sent there
// becomes an item in the collection selected in the library, as in Zotero.
//
//   GET  /connector/ping             {app, version, api}
//   POST /connector/save             {item}: a Zotero-style item read from the page
//   POST /connector/attachment       a PDF's bytes; ?parent=&library=&url=&title=
//   POST /connector/attachmentUrl    {parent, library, url, title}: OurNotes fetches it
//   POST /connector/update           {key, library, target, tags}: another collection
//
// Requests are served on their own thread; the library is used on the GUI thread.
class ConnectorServer : public QObject {
    Q_OBJECT
public:
    static constexpr int kDefaultPort = 23120;  // Zotero's connector uses 23119

    explicit ConnectorServer(QObject* parent = nullptr);
    ~ConnectorServer() override;

    bool start(int port = kDefaultPort);  // false: the port is taken (another OurNotes?)
    void stop();
    bool isRunning() const { return thread_.joinable(); }
    int port() const { return port_; }

    // Where saved pages go: the library's selection (a library or collection).
    std::function<Scope()> targetScope;

signals:
    void saved(qint64 libraryId, const QString& key, const QString& title);
    // A PDF saved on its own (from a PDF tab): its details are to be looked up.
    void metadataWanted(qint64 libraryId, const QStringList& attachmentKeys);

private:
    Json save(const Json& item);
    Json addPdf(const fs::path& file, int64_t libraryId, const std::string& parentKey, const std::string& url,
                const std::string& title);
    Json update(const Json& body);
    Json targets(int64_t libraryId) const;

    // Runs f on the GUI thread and waits for it (gives up when the server stops).
    template <typename F>
    auto onGui(F f) -> decltype(f());

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    int port_ = 0;
};

// The item as OurNotes keeps it: a known type (else "webpage"), only that type's
// fields, creators and tags in Zotero's form, an access date. Exposed for tests.
Json connectorItem(const Json& item);

} // namespace on::gui
