#include "gui/fulltext_indexer.h"

#include <QMetaObject>

#include <chrono>

#include "core/fulltext.h"
#include "core/schema.h"
#include "gui/app_context.h"
#include "pdf/document.h"

namespace fp::gui {

FulltextIndexer::FulltextIndexer(QObject* parent) : QObject(parent)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { run(false); });
    connect(&AppContext::get(), &AppContext::changed, this,
            [this](qint64, const QString& type, const QString& action, const QStringList&) {
                if (type == "item" && action == "add")
                    schedule(10000);
            });
}

FulltextIndexer::~FulltextIndexer()
{
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

void FulltextIndexer::schedule(int delayMs)
{
    if (!AppContext::get().config().get("/fulltext/enabled", true).get<bool>())
        return;
    timer_.start(delayMs);
}

void FulltextIndexer::rebuild() { run(true); }

void FulltextIndexer::run(bool clear)
{
    if (running_) {
        schedule(10000);
        return;
    }
    if (thread_.joinable())
        thread_.join();
    running_ = true;
    stop_ = false;
    auto dataDir = AppContext::get().library().dataDir();
    thread_ = std::thread([this, dataDir, clear] {
        int indexed = 0;
        try {
            Library lib(dataDir);
            auto& index = lib.fulltext();
            if (clear)
                index.clear();
            struct Job {
                int64_t lib;
                Json att;
            };
            std::vector<Job> jobs;
            for (auto& info : lib.libraries())
                for (auto& d : lib.allItems(info.id, true)) {
                    if (jstr(d, "itemType") != "attachment")
                        continue;
                    auto ct = jstr(d, "contentType");
                    if (ct != "application/pdf" && ct != "text/html" && ct != "text/plain" &&
                        ct != "application/epub+zip")
                        continue;
                    jobs.push_back({info.id, d});
                }
            int done = 0;
            for (auto& job : jobs) {
                if (stop_)
                    break;
                ++done;
                auto path = lib.attachmentFile(job.lib, job.att);
                std::error_code ec;
                if (path.empty() || !fs::is_regular_file(path, ec))
                    continue;
                auto key = jstr(job.att, "key");
                auto md5 = lib.fileMd5(job.lib, key, path);
                if (index.isIndexed(job.lib, key, md5))
                    continue;
                std::string text;
                int pages = 0;
                try {
                    auto ct = jstr(job.att, "contentType");
                    if (ct == "application/pdf" || ct == "application/epub+zip") {
                        auto doc = pdf::Document::open(path);
                        pages = doc->pageCount();
                        text = doc->fullText();
                    } else if (ct == "text/html") {
                        text = htmlToText(readFile(path));
                    } else {
                        text = readFile(path);
                    }
                } catch (const std::exception&) {
                    text.clear();
                }
                index.store(job.lib, key, md5, text, pages);
                ++indexed;
                QMetaObject::invokeMethod(
                    this, [this, done, total = (int)jobs.size()] { emit progress(done, total); },
                    Qt::QueuedConnection);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (const std::exception&) {
        }
        QMetaObject::invokeMethod(
            this,
            [this, indexed] {
                running_ = false;
                emit finished(indexed);
            },
            Qt::QueuedConnection);
    });
}

} // namespace fp::gui
