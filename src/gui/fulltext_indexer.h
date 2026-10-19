#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <thread>

namespace on::gui {

// Extracts text from PDF/HTML/text attachments in the background so the
// "Everything" search mode can find words inside documents.
class FulltextIndexer : public QObject {
    Q_OBJECT
public:
    explicit FulltextIndexer(QObject* parent = nullptr);
    ~FulltextIndexer() override;

    void schedule(int delayMs = 5000);
    void rebuild();

signals:
    void progress(int done, int total);
    void finished(int indexed);

private:
    void run(bool clear);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    QTimer timer_;
};

} // namespace on::gui
