#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "formats/pdf_metadata.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace fp::gui {

class FancyProgressBar;

// Non-modal window that retrieves metadata for standalone PDFs, showing per-file
// status and overall progress. Found items become parents of their PDFs as soon
// as each lookup completes.
class MetadataProgressDialog : public QDialog {
    Q_OBJECT
public:
    struct Job {
        QString key;  // attachment item
        QString name;
        fs::path path;
    };
    using Retriever = std::function<std::optional<formats::PdfMetadataMatch>(
        const fs::path&, const std::function<void(const std::string&)>&)>;

    MetadataProgressDialog(int64_t libraryId, std::vector<Job> jobs, QWidget* parent = nullptr,
                           Retriever retriever = {});
    ~MetadataProgressDialog() override;

    void start();
    bool isFinished() const { return finished_; }
    int foundCount() const { return found_; }
    int notFoundCount() const { return missed_; }

signals:
    void itemActivated(qint64 libraryId, const QString& key);

protected:
    void closeEvent(QCloseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;

private:
    struct Shared;
    void onStage(int index, const QString& stage);
    void onResult(int index, const std::optional<formats::PdfMetadataMatch>& match, const QString& error);
    void updateSummary();
    void finish();
    void animateSpinners();

    int64_t libraryId_;
    std::vector<Job> jobs_;
    std::vector<QString> parents_;  // created parent item per job
    std::vector<bool> running_;
    Retriever retriever_;
    std::shared_ptr<Shared> shared_;
    QLabel* title_;
    QLabel* subtitle_;
    FancyProgressBar* bar_;
    QTreeWidget* list_;
    QLabel* summary_;
    QPushButton* button_;
    QTimer spinner_;
    QTimer autoClose_;
    QElapsedTimer clock_;
    int frame_ = 0;
    int done_ = 0;
    int found_ = 0;
    int missed_ = 0;
    int closeCountdown_ = 0;
    bool finished_ = false;
    bool userInteracted_ = false;
};

} // namespace fp::gui
