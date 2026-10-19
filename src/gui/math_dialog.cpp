#include "gui/math_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QVBoxLayout>

#include "gui/math_render.h"

namespace fp::gui {

MathDialog::MathDialog(const QString& latex, bool display, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Formula"));
    resize(560, 360);
    auto* v = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("LaTeX math, e.g. <code>\\frac{a}{b}</code>, <code>\\sum_{i=1}^n x_i</code>, "
                               "<code>\\mathbb{R}^n</code>"),
                            this);
    hint->setWordWrap(true);
    v->addWidget(hint);
    source_ = new QPlainTextEdit(latex, this);
    source_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    source_->setTabChangesFocus(true);
    source_->setFixedHeight(source_->fontMetrics().lineSpacing() * 4 + 12);
    source_->installEventFilter(this);
    v->addWidget(source_);
    display_ = new QCheckBox(tr("Display formula (on its own centred line)"), this);
    display_->setChecked(display);
    v->addWidget(display_);
    auto* area = new QScrollArea(this);
    area->setWidgetResizable(true);
    area->setAlignment(Qt::AlignCenter);
    preview_ = new QLabel(area);
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setBackgroundRole(QPalette::Base);
    preview_->setAutoFillBackground(true);
    area->setWidget(preview_);
    v->addWidget(area, 1);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    timer_.setSingleShot(true);
    timer_.setInterval(120);
    connect(&timer_, &QTimer::timeout, this, &MathDialog::updatePreview);
    connect(source_, &QPlainTextEdit::textChanged, &timer_, qOverload<>(&QTimer::start));
    connect(display_, &QCheckBox::toggled, this, &MathDialog::updatePreview);
    updatePreview();
    source_->setFocus();
    source_->moveCursor(QTextCursor::End);
}

QString MathDialog::latex() const { return source_->toPlainText().trimmed(); }
bool MathDialog::isDisplay() const { return display_->isChecked(); }

bool MathDialog::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == source_ && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && (ke->modifiers() & Qt::ControlModifier)) {
            accept();
            return true;
        }
    }
    return QDialog::eventFilter(watched, e);
}

void MathDialog::updatePreview()
{
    auto tex = latex();
    if (tex.isEmpty()) {
        preview_->clear();
        status_->setText(tr("Leave empty to remove the formula."));
        return;
    }
    auto m = renderMath(tex, isDisplay(), 26, palette().color(QPalette::Text), devicePixelRatioF());
    preview_->setPixmap(QPixmap::fromImage(m.image));
    status_->setText(m.ok ? QString() : "<span style=\"color:#c0392b\">" + m.error.toHtmlEscaped() + "</span>");
}

} // namespace fp::gui
