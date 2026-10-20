#include "gui/search_field.h"

#include <QAction>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

#include "gui/app_context.h"
#include "gui/common.h"

namespace on::gui {

namespace {
constexpr int kMaxHistory = 1000;
constexpr int kListRows = 5;  // shown at once; PgDn and PgUp go on by as many
} // namespace

SearchField::SearchField(QWidget* parent, const QString& historyKey) : QLineEdit(parent), historyKey_(historyKey)
{
    if (auto* ctx = AppContext::tryGet()) {
        auto saved = ctx->config().get(ss(historyKey_), Json::array());
        if (saved.is_array())
            for (auto& s : saved)
                if (s.is_string())
                    history_ << qs(s.get<std::string>());
    }
    connect(this, &QLineEdit::editingFinished, this, [this] { remember(text()); });
    connect(this, &QLineEdit::textEdited, this, [this] {
        browsing_ = -1;  // typing again: Up starts from this
        typedByUser_ = true;
        if (isListShown())
            hideList(false);
    });
}

void SearchField::addCaseToggle(const QString& key)
{
    caseKey_ = key;
    if (auto* ctx = AppContext::tryGet())
        caseSensitive_ = ctx->config().get(ss(key), false).get<bool>();
    caseButton_ = addAction(QIcon(), QLineEdit::TrailingPosition);
    caseButton_->setObjectName("matchCase");
    connect(caseButton_, &QAction::triggered, this, [this] { setCaseSensitive(!caseSensitive_); });
    updateCaseButton();
}

void SearchField::setCaseSensitive(bool on)
{
    if (on == caseSensitive_)
        return;
    caseSensitive_ = on;
    if (auto* ctx = AppContext::tryGet(); ctx && !caseKey_.isEmpty())
        ctx->config().set(ss(caseKey_), on);
    updateCaseButton();
    emit caseSensitivityChanged(on);
}

// "Aa": faint when off; on, in the highlight's colours (the field's buttons show no
// pressed state of their own)
void SearchField::updateCaseButton()
{
    if (!caseButton_)
        return;
    qreal dpr = devicePixelRatioF();
    QPixmap pm(QSize(18, 16) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF box(0, 0, 18, 16);
    if (caseSensitive_) {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Highlight));
        p.drawRoundedRect(box, 3, 3);
    }
    QFont f = font();
    f.setPixelSize(11);
    f.setBold(caseSensitive_);
    p.setFont(f);
    p.setPen(palette().color(caseSensitive_ ? QPalette::HighlightedText : QPalette::PlaceholderText));
    p.drawText(box, Qt::AlignCenter, "Aa");
    p.end();
    caseButton_->setIcon(QIcon(pm));
    caseButton_->setToolTip(caseSensitive_ ? tr("Matching case (Alt+C): click to match any case")
                                           : tr("Match case (Alt+C)"));
}

void SearchField::changeEvent(QEvent* e)
{
    QLineEdit::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        updateCaseButton();
}

void SearchField::remember(const QString& text)
{
    auto t = text.trimmed();
    typedByUser_ = false;  // kept: no longer what Up looks for
    if (t.isEmpty())
        return;
    history_.removeAll(t);
    history_ << t;
    while (history_.size() > kMaxHistory)
        history_.removeFirst();
    saveHistory();
}

void SearchField::saveHistory()
{
    if (auto* ctx = AppContext::tryGet()) {
        Json saved = Json::array();
        for (auto& s : history_)
            saved.push_back(ss(s));
        ctx->config().set(ss(historyKey_), saved);
    }
}

QStringList SearchField::matching(const QString& prefix) const
{
    QStringList out;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        if (it->startsWith(prefix, Qt::CaseInsensitive) && it->compare(prefix, Qt::CaseInsensitive) != 0 &&
            !out.contains(*it))
            out << *it;
    return out;
}

void SearchField::fill(const QString& text)
{
    setText(text);
    setCursorPosition(int(text.size()));
}

void SearchField::browse(int step)
{
    if (browsing_ < 0 && step < 0)
        return;  // at what was typed already
    if (browsing_ < 0) {
        typed_ = text();
        browsed_ = matching(typedByUser_ ? typed_ : QString());
        browsed_.removeAll(typed_);  // (what is shown already)
    }
    int next = browsing_ + step;
    if (next >= browsed_.size())
        return;  // the oldest: it stays
    browsing_ = std::max(-1, next);
    fill(browsing_ < 0 ? typed_ : browsed_[browsing_]);
}

bool SearchField::complete()
{
    auto text = this->text();
    auto found = matching(text);
    if (found.isEmpty())
        return false;
    if (found.size() == 1) {
        fill(found.front());
        return true;
    }
    // as far as they all agree (in the latest one's spelling)
    QString common = found.front();
    for (auto& f : found) {
        int n = 0;
        while (n < common.size() && n < f.size() && common[n].toLower() == f[n].toLower())
            ++n;
        common.truncate(n);
    }
    if (common.size() > text.size()) {
        fill(common);
        return true;
    }
    typed_ = text;
    showList(found);  // nothing more to complete: the list, as a second Tab in zsh
    return true;
}

void SearchField::showList(const QStringList& searches)
{
    if (!popup_) {
        popup_ = new QFrame(window());
        popup_->setObjectName("searchHistoryList");
        popup_->setFrameShape(QFrame::StyledPanel);
        popup_->setAutoFillBackground(true);
        popup_->setBackgroundRole(QPalette::Base);
        auto* v = new QVBoxLayout(popup_);
        v->setContentsMargins(1, 1, 1, 1);
        v->setSpacing(0);
        list_ = new QListWidget(popup_);
        list_->setFrameShape(QFrame::NoFrame);
        list_->setFocusPolicy(Qt::NoFocus);  // the keys stay in the field
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // the count says where
        v->addWidget(list_);
        count_ = new QLabel(popup_);
        count_->setObjectName("searchHistoryCount");
        count_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        count_->setContentsMargins(6, 1, 6, 2);
        count_->setForegroundRole(QPalette::PlaceholderText);
        v->addWidget(count_);
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            fill(item->text());
            hideList(false);
            setFocus();
        });
    }
    list_->clear();
    list_->addItems(searches);
    count_->setVisible(searches.size() > kListRows);
    int rows = std::min<int>(kListRows, int(searches.size()));
    list_->setFixedHeight(rows * list_->sizeHintForRow(0));
    popup_->setFixedWidth(width());
    popup_->adjustSize();
    // under the field, or over it when there is no room below (a note's find, at the bottom)
    QPoint below = mapTo(window(), QPoint(0, height()));
    QPoint above = mapTo(window(), QPoint(0, -popup_->height()));
    popup_->move(below.y() + popup_->height() > window()->height() && above.y() >= 0 ? above : below);
    popup_->raise();
    popup_->show();
    list_->setCurrentRow(-1);  // (a list shown again would go on from where it was)
    moveInList(1);             // the first one, highlighted and in the field
}

void SearchField::hideList(bool backToTyped)
{
    if (!isListShown())
        return;
    popup_->hide();
    if (backToTyped)
        fill(typed_);
}

bool SearchField::isListShown() const { return popup_ && popup_->isVisible(); }

QStringList SearchField::listed() const
{
    QStringList out;
    if (isListShown())
        for (int i = 0; i < list_->count(); ++i)
            out << list_->item(i)->text();
    return out;
}

int SearchField::listedCurrent() const { return isListShown() ? list_->currentRow() : -1; }

QString SearchField::listedCount() const { return isListShown() && count_->isVisible() ? count_->text() : QString(); }

void SearchField::moveInList(int step, bool wrap)
{
    int n = list_->count();
    if (n == 0)
        return;
    int row = list_->currentRow();
    if (row < 0)
        row = step > 0 ? 0 : n - 1;
    else if (wrap)
        row = ((row + step) % n + n) % n;
    else
        row = std::clamp(row + step, 0, n - 1);
    list_->setCurrentRow(row);
    // the five shown hold it: whole pages of five, as PgDn and PgUp go
    list_->scrollToItem(list_->item(row / kListRows * kListRows), QAbstractItemView::PositionAtTop);
    count_->setText(QString("%1/%2").arg(row + 1).arg(n));
    fill(list_->item(row)->text());
}

void SearchField::forgetCurrent()
{
    int row = list_->currentRow();
    if (row < 0)
        return;
    history_.removeAll(list_->item(row)->text());
    saveHistory();
    delete list_->takeItem(row);
    if (list_->count() == 0) {
        hideList(true);
        return;
    }
    QStringList rest;
    for (int i = 0; i < list_->count(); ++i)
        rest << list_->item(i)->text();
    showList(rest);  // sized and counted again
    moveInList(std::min(row, int(rest.size()) - 1), false);  // the next one, where it was
}

bool SearchField::event(QEvent* e)
{
    // Ctrl+F here: this field's (not another find, a PDF's, the window's)
    if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->matches(QKeySequence::Find)) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->matches(QKeySequence::Find)) {
        hideList(false);
        selectAll();
        return true;
    }
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        bool tab = k->key() == Qt::Key_Tab && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
        bool backtab = k->key() == Qt::Key_Backtab || (tab && (k->modifiers() & Qt::ShiftModifier));
        if (isListShown() && (tab || backtab)) {
            moveInList(backtab ? -1 : 1);
            return true;
        }
        if (tab && !backtab) {
            if (text().isEmpty()) {  // nothing typed (just after Ctrl+F, say): all of them
                typed_.clear();
                if (auto all = matching({}); !all.isEmpty())
                    showList(all);
            } else {
                complete();
            }
            return true;  // for completing here, not for moving on (also after a completion)
        }
    }
    return QLineEdit::event(e);
}

void SearchField::keyPressEvent(QKeyEvent* e)
{
    bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    bool enter = e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
    if (isListShown()) {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
            moveInList(e->key() == Qt::Key_Down ? 1 : -1);
            return;
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            moveInList(e->key() == Qt::Key_PageDown ? kListRows : -kListRows, false);
            return;
        case Qt::Key_Backspace:
            if (e->modifiers() & Qt::ControlModifier) {
                forgetCurrent();
                return;
            }
            break;
        case Qt::Key_Escape:
            hideList(true);
            return;
        default:
            break;
        }
        if (enter)
            hideList(false);  // and searched for, as Enter does
    } else if (plain && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)) {
        browse(e->key() == Qt::Key_Up ? 1 : -1);
        return;
    } else if (e->key() == Qt::Key_Escape) {
        browsing_ = -1;
        emit escapePressed();
        return;
    }
    if (caseButton_ && e->key() == Qt::Key_C && e->modifiers() == Qt::AltModifier) {
        setCaseSensitive(!caseSensitive_);
        return;
    }
    if (enter && (e->modifiers() & Qt::ShiftModifier)) {
        remember(text());
        emit shiftReturnPressed();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void SearchField::focusOutEvent(QFocusEvent* e)
{
    hideList(false);
    QLineEdit::focusOutEvent(e);
}

} // namespace on::gui
