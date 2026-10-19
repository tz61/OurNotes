#include "gui/reader/render_worker.h"

#include <memory>

#include "gui/theme.h"
#include "pdf/document.h"

namespace on::gui {

RenderWorker::RenderWorker(const fs::path& path, QObject* parent) : QObject(parent), path_(path)
{
    thread_ = std::thread([this] { run(); });
}

RenderWorker::~RenderWorker()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void RenderWorker::setQueue(std::deque<Request> requests)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(requests);
    }
    cv_.notify_all();
}

void RenderWorker::addRequests(const std::deque<Request>& requests)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        background_.insert(background_.end(), requests.begin(), requests.end());
    }
    cv_.notify_all();
}

void RenderWorker::run()
{
    std::unique_ptr<pdf::Document> doc;
    try {
        doc = pdf::Document::open(path_);
    } catch (const std::exception&) {
        return;
    }
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty() || !background_.empty(); });
            if (stop_)
                return;
            if (!queue_.empty()) {
                req = queue_.front();
                queue_.pop_front();
            } else {
                req = background_.front();
                background_.pop_front();
            }
        }
        if (req.page < 0 || req.page >= doc->pageCount())
            continue;
        try {
            auto img = doc->render(req.page, (float)req.scale);
            QImage image = QImage(img.pixels.data(), img.width, img.height, img.stride, QImage::Format_RGB888).copy();
            if (req.dark)
                darkenPage(image, doc->imageRects(req.page), req.scale, doc->pageSize(req.page));
            emit rendered(req.page, req.scale, req.generation, req.thumbnail, req.dark, image);
        } catch (const std::exception&) {
            QImage blank(1, 1, QImage::Format_RGB888);
            blank.fill(req.dark ? darkPageColor() : QColor(Qt::white));
            emit rendered(req.page, req.scale, req.generation, req.thumbnail, req.dark, blank);
        }
    }
}

} // namespace on::gui
