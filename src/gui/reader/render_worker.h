#pragma once

#include <QImage>
#include <QObject>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/util.h"

namespace on::gui {

// Renders pages on a background thread with its own MuPDF document.
// The request queue is replaced wholesale, so scrolling quickly never renders
// pages that are no longer needed.
class RenderWorker : public QObject {
    Q_OBJECT
public:
    struct Request {
        int page = 0;
        double scale = 1;  // device pixels per point
        quint64 generation = 0;
        bool thumbnail = false;
        bool dark = false;  // recolour for dark mode
    };

    explicit RenderWorker(const fs::path& path, QObject* parent = nullptr);
    ~RenderWorker() override;

    void setQueue(std::deque<Request> requests);
    // Appended, e.g. thumbnails; a page asked for again replaces its waiting request.
    void addRequests(const std::deque<Request>& requests);

signals:
    void rendered(int page, double scale, quint64 generation, bool thumbnail, bool dark, const QImage& image);

private:
    void run();

    fs::path path_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::deque<Request> background_;
    Request busy_;           // being rendered, while rendering_
    bool rendering_ = false;
    bool stop_ = false;
};

} // namespace on::gui
