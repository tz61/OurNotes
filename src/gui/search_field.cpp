#include "gui/search_field.h"

#include <QKeyEvent>
#include <QListWidget>

#include <algorithm>

#include "gui/app_context.h"
#include "gui/common.h"

namespace on::gui {

namespace {
constexpr const char* kHistoryKey = "/ui/searchHistory";
constexpr int kMaxHistory = 100;
constexpr int kMaxListed = 8;  // rows shown at once; the list scrolls for more
} // namespace

SearchField::SearchField(QWidget* parent) : QLineEdit(parent)
{
    if (auto* ctx = AppContext::tryGet()) {
        auto saved = ctx->config().get(kHistoryKey, Json::array());
        if (saved.is_array())
            for (auto& s : saved)
                if (s.is_string())
                    history_ << qs(s.get<std::string>());
    }
    connect(this, &QLineEdit::editingFinished, this, [this] { remember(text()); });
    connect(this, &QLineEdit::textEdited, this, [this] {
        browsing_ = -1;  // typing again: Up starts from this
        if (isListShown())
            hideList(false);
    });
}

void SearchField::remember(const QString& text)
{
    auto t = text.trimmed();
    if (t.isEmpty())
        return;
    history_.removeAll(t);
    history_ << t;
    while (history_.size() > kMaxHistory)
        history_.removeFirst();
    if (auto* ctx = AppContext::tryGet()) {
        Json saved = Json::array();
        for (auto& s : history_)
            saved.push_back(ss(s));
        ctx->config().set(kHistoryKey, saved);
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
        browsed_ = matching(typed_);
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
    if (!list_) {
        list_ = new QListWidget(window());
        list_->setObjectName("searchHistoryList");
        list_->setFocusPolicy(Qt::NoFocus);  // the keys stay in the field
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            fill(item->text());
            hideList(false);
            setFocus();
        });
    }
    list_->clear();
    list_->addItems(searches);
    int rows = std::min<int>(kMaxListed, int(searches.size()));
    list_->setFixedSize(width(), rows * list_->sizeHintForRow(0) + 2 * list_->frameWidth());
    list_->move(mapTo(window(), QPoint(0, height())));
    list_->raise();
    list_->show();
    list_->setCurrentRow(-1);  // (a list shown again would go on from where it was)
    moveInList(1);  // the first one, highlighted and in the field
}

void SearchField::hideList(bool backToTyped)
{
    if (!isListShown())
        return;
    list_->hide();
    if (backToTyped)
        fill(typed_);
}

bool SearchField::isListShown() const { return list_ && list_->isVisible(); }

QStringList SearchField::listed() const
{
    QStringList out;
    if (isListShown())
        for (int i = 0; i < list_->count(); ++i)
            out << list_->item(i)->text();
    return out;
}

int SearchField::listedCurrent() const { return isListShown() ? list_->currentRow() : -1; }

void SearchField::moveInList(int step)
{
    int n = list_->count();
    if (n == 0)
        return;
    int row = list_->currentRow() < 0 ? (step > 0 ? 0 : n - 1) : (list_->currentRow() + step + n) % n;
    list_->setCurrentRow(row);
    list_->scrollToItem(list_->item(row));
    fill(list_->item(row)->text());
}

bool SearchField::event(QEvent* e)
{
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        bool tab = k->key() == Qt::Key_Tab && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier));
        bool backtab = k->key() == Qt::Key_Backtab || (tab && (k->modifiers() & Qt::ShiftModifier));
        if (isListShown() && (tab || backtab)) {
            moveInList(backtab ? -1 : 1);
            return true;
        }
        if (tab && !backtab && complete())
            return true;
    }
    return QLineEdit::event(e);
}

void SearchField::keyPressEvent(QKeyEvent* e)
{
    bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (isListShown()) {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
            moveInList(e->key() == Qt::Key_Down ? 1 : -1);
            return;
        case Qt::Key_Escape:
            hideList(true);
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            hideList(false);
            break;  // and searched for, as Enter does
        default:
            break;
        }
    } else if (plain && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)) {
        browse(e->key() == Qt::Key_Up ? 1 : -1);
        return;
    } else if (e->key() == Qt::Key_Escape) {
        browsing_ = -1;
        emit escapePressed();
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
