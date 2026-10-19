#include "gui/metadata_progress.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <atomic>
#include <cmath>
#include <thread>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/fancy_progress.h"
#include "gui/formats_actions.h"

namespace fp::gui {

namespace {

constexpr int kWorkers = 3;
constexpr int kIconSize = 18;

enum class Status { Pending, Found, Missing };

QPixmap canvas()
{
    qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(int(kIconSize * dpr), int(kIconSize * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    return pm;
}

QIcon statusIcon(Status status)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(2, 2, kIconSize - 4, kIconSize - 4);
    if (status == Status::Pending) {
        p.setPen(QPen(QColor(150, 155, 165), 1.6));
        p.drawEllipse(r);
        return QIcon(pm);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(status == Status::Found ? QColor("#2fa85a") : QColor("#e8892b"));
    p.drawEllipse(r);
    p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (status == Status::Found) {
        QPainterPath check;
        check.moveTo(r.left() + r.width() * 0.27, r.top() + r.height() * 0.53);
        check.lineTo(r.left() + r.width() * 0.44, r.top() + r.height() * 0.7);
        check.lineTo(r.left() + r.width() * 0.74, r.top() + r.height() * 0.33);
        p.setBrush(Qt::NoBrush);
        p.drawPath(check);
    } else {
        p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.25),
                   QPointF(r.center().x(), r.top() + r.height() * 0.58));
        p.drawPoint(QPointF(r.center().x(), r.top() + r.height() * 0.76));
    }
    return QIcon(pm);
}

QIcon spinnerIcon(int frame)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(3, 3, kIconSize - 6, kIconSize - 6);
    p.setPen(QPen(QColor(61, 127, 240, 50), 2.4));
    p.drawEllipse(r);
    QConicalGradient g(r.center(), -frame * 30.0);
    g.setColorAt(0, QColor("#3d7ff0"));
    g.setColorAt(0.75, QColor(61, 127, 240, 0));
    p.setPen(QPen(QBrush(g), 2.4, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(r, int(-frame * 30.0 * 16), 270 * 16);
    return QIcon(pm);
}

QString formatSeconds(double s)
{
    if (s < 1)
        return QObject::tr("under a second");
    if (s < 60)
        return QObject::tr("%1 s").arg(std::lround(s));
    return QObject::tr("%1 min").arg(std::lround(s / 60));
}

} // namespace

struct MetadataProgressDialog::Shared {
    std::atomic<bool> cancel{false};
    std::atomic<int> next{0};
};

MetadataProgressDialog::MetadataProgressDialog(int64_t libraryId, std::vector<Job> jobs, QWidget* parent,
                                               Retriever retriever)
    : QDialog(parent), libraryId_(libraryId), jobs_(std::move(jobs)), parents_(jobs_.size()),
      running_(jobs_.size(), false), retriever_(std::move(retriever)), shared_(std::make_shared<Shared>())
{
    if (!retriever_)
        retriever_ = [](const fs::path& p, const std::function<void(const std::string&)>& stage) {
            return formats::retrieveMetadataForPdf(p, stage);
        };
    setWindowTitle(tr("Retrieve Metadata"));
    setModal(false);
    resize(680, std::min(560, 230 + 30 * (int)jobs_.size()));

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 16, 18, 14);
    v->setSpacing(8);
    title_ = new QLabel(tr("Retrieving metadata…"), this);
    QFont tf = title_->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    tf.setBold(true);
    title_->setFont(tf);
    subtitle_ = new QLabel(this);
    // secondary text: between the text and background colours
    QPalette dim = subtitle_->palette();
    QColor fg = dim.color(QPalette::WindowText), bg = dim.color(QPalette::Window);
    dim.setColor(QPalette::WindowText, QColor::fromRgbF(fg.redF() * 0.62 + bg.redF() * 0.38,
                                                        fg.greenF() * 0.62 + bg.greenF() * 0.38,
                                                        fg.blueF() * 0.62 + bg.blueF() * 0.38));
    subtitle_->setPalette(dim);
    bar_ = new FancyProgressBar(this);
    bar_->setMaximum((int)jobs_.size());
    v->addWidget(title_);
    v->addWidget(subtitle_);
    v->addSpacing(2);
    v->addWidget(bar_);
    v->addSpacing(4);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(3);
    list_->setHeaderLabels({tr("PDF"), tr("Item"), tr("Found via")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setIconSize(QSize(kIconSize, kIconSize));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    list_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list_->header()->resizeSection(0, 220);
    for (auto& job : jobs_) {
        auto* row = new QTreeWidgetItem(list_);
        row->setIcon(0, statusIcon(Status::Pending));
        row->setText(0, job.name);
        row->setToolTip(0, job.name);
        row->setText(1, tr("Waiting"));
        row->setForeground(1, palette().color(QPalette::Disabled, QPalette::Text));
    }
    v->addWidget(list_, 1);

    auto* footer = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setPalette(dim);
    button_ = new QPushButton(tr("Cancel"), this);
    footer->addWidget(summary_, 1);
    footer->addWidget(button_);
    v->addLayout(footer);

    connect(button_, &QPushButton::clicked, this, &QDialog::close);
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
        userInteracted_ = true;
        int i = list_->indexOfTopLevelItem(row);
        if (i >= 0 && !parents_[i].isEmpty())
            emit itemActivated(libraryId_, parents_[i]);
    });
    spinner_.setInterval(70);
    connect(&spinner_, &QTimer::timeout, this, &MetadataProgressDialog::animateSpinners);
    autoClose_.setInterval(1000);
    connect(&autoClose_, &QTimer::timeout, this, [this] {
        if (--closeCountdown_ <= 0) {
            autoClose_.stop();
            close();
            return;
        }
        summary_->setText(tr("All done · closing in %1 s").arg(closeCountdown_));
    });
    updateSummary();
}

MetadataProgressDialog::~MetadataProgressDialog() { shared_->cancel = true; }

void MetadataProgressDialog::start()
{
    clock_.start();
    spinner_.start();
    QPointer<MetadataProgressDialog> guard(this);
    std::vector<fs::path> paths;
    for (auto& j : jobs_)
        paths.push_back(j.path);
    int workers = std::min<int>(kWorkers, (int)jobs_.size());
    for (int w = 0; w < workers; ++w) {
        std::thread([guard, shared = shared_, paths, retriever = retriever_] {
            for (;;) {
                if (shared->cancel)
                    return;
                int i = shared->next++;
                if (i >= (int)paths.size())
                    return;
                auto post = [&](auto fn) { QMetaObject::invokeMethod(qApp, fn, Qt::QueuedConnection); };
                post([guard, i] {
                    if (guard)
                        guard->onStage(i, tr("Starting"));
                });
                std::optional<formats::PdfMetadataMatch> match;
                QString error;
                try {
                    match = retriever(paths[i], [&](const std::string& stage) {
                        post([guard, i, s = qs(stage)] {
                            if (guard)
                                guard->onStage(i, s);
                        });
                    });
                } catch (const std::exception& e) {
                    error = qs(e.what());
                }
                post([guard, i, match, error] {
                    if (guard)
                        guard->onResult(i, match, error);
                });
            }
        }).detach();
    }
}

void MetadataProgressDialog::onStage(int index, const QString& stage)
{
    if (shared_->cancel)
        return;
    running_[index] = true;
    auto* row = list_->topLevelItem(index);
    row->setText(1, stage + "…");
    QFont f = row->font(1);
    f.setItalic(true);
    row->setFont(1, f);
    row->setForeground(1, palette().color(QPalette::Disabled, QPalette::Text));
    row->setIcon(0, spinnerIcon(frame_));
}

void MetadataProgressDialog::onResult(int index, const std::optional<formats::PdfMetadataMatch>& match,
                                      const QString& error)
{
    running_[index] = false;
    auto* row = list_->topLevelItem(index);
    QFont f = row->font(1);
    f.setItalic(false);
    row->setFont(1, f);
    QString parent;
    if (match && !shared_->cancel)
        parent = applyMetadataMatch(libraryId_, jobs_[index].key, match->item);
    if (!parent.isEmpty()) {
        parents_[index] = parent;
        ++found_;
        row->setIcon(0, statusIcon(Status::Found));
        row->setText(1, jqs(match->item, "title"));
        row->setToolTip(1, jqs(match->item, "title") + "\n" + tr("Double-click to show it in the library"));
        row->setForeground(1, palette().color(QPalette::Text));
        row->setText(2, qs(match->source));
    } else {
        ++missed_;
        row->setIcon(0, statusIcon(Status::Missing));
        row->setText(1, error.isEmpty() ? tr("No metadata found") : error);
        row->setForeground(1, QColor("#c26a12"));
    }
    ++done_;
    bar_->setValue(done_);
    updateSummary();
    if (done_ == (int)jobs_.size())
        finish();
}

void MetadataProgressDialog::animateSpinners()
{
    frame_ = (frame_ + 1) % 12;
    auto icon = spinnerIcon(frame_);
    for (size_t i = 0; i < running_.size(); ++i)
        if (running_[i])
            list_->topLevelItem((int)i)->setIcon(0, icon);
}

void MetadataProgressDialog::updateSummary()
{
    int total = (int)jobs_.size();
    if (!finished_) {
        QString text = total == 1 ? tr("Looking up 1 PDF") : tr("%1 of %2 PDFs done").arg(done_).arg(total);
        if (done_ > 0 && done_ < total) {
            double left = clock_.elapsed() / 1000.0 / done_ * (total - done_);
            text += " · " + (left < 1.5 ? tr("almost done") : tr("about %1 left").arg(formatSeconds(left)));
        }
        subtitle_->setText(text);
    }
    QStringList parts;
    if (found_)
        parts << tr("%n found", nullptr, found_);
    if (missed_)
        parts << tr("%n not found", nullptr, missed_);
    summary_->setText(parts.isEmpty() ? tr("Only the identifiers or titles are sent to Crossref, arXiv, "
                                           "Open Library or PubMed — never the PDF.")
                                      : parts.join(" · "));
}

void MetadataProgressDialog::finish()
{
    finished_ = true;
    spinner_.stop();
    bar_->setState(missed_ ? FancyProgressBar::State::Warning : FancyProgressBar::State::Done);
    title_->setText(missed_ ? tr("Metadata retrieved for %1 of %2").arg(found_).arg(jobs_.size())
                            : tr("Metadata retrieved"));
    subtitle_->setText(tr("Finished in %1").arg(formatSeconds(clock_.elapsed() / 1000.0)));
    button_->setText(tr("Close"));
    button_->setDefault(true);
    updateSummary();
    if (!missed_ && !userInteracted_) {
        closeCountdown_ = 4;
        summary_->setText(tr("All done · closing in %1 s").arg(closeCountdown_));
        autoClose_.start();
    } else if (missed_) {
        summary_->setText(summary_->text() + " · " +
                          tr("For PDFs without metadata, create an item and drag the PDF onto it."));
    }
}

void MetadataProgressDialog::enterEvent(QEnterEvent* e)
{
    QDialog::enterEvent(e);
    userInteracted_ = true;
    if (autoClose_.isActive()) {
        autoClose_.stop();
        updateSummary();
    }
}

void MetadataProgressDialog::closeEvent(QCloseEvent* e)
{
    shared_->cancel = true;  // workers stop taking new files; late results are ignored
    QDialog::closeEvent(e);
}

} // namespace fp::gui
