#include "gui/reader/translation_popup.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/icons.h"

namespace on::gui {

namespace {

constexpr int kMaxWidth = 440;
constexpr int kMaxHeight = 360;

bool isCjk(QChar c)
{
    auto u = c.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xAC00 && u <= 0xD7AF) || (u >= 0xF900 && u <= 0xFAFF) ||
           (u >= 0xFF00 && u <= 0xFFEF);
}

bool isLineBreak(QChar c)
{
    return c == '\n' || c == '\r' || c == QChar::LineSeparator || c == QChar::ParagraphSeparator;
}

QString dim(const QWidget* w) { return w->palette().color(QPalette::PlaceholderText).name(); }

} // namespace

TranslationPopup::Translator& TranslationPopup::translator()
{
    static Translator t = [](const std::string& text, const std::string& target) {
        return formats::translate(text, target);
    };
    return t;
}

QString TranslationPopup::targetLanguage()
{
    if (auto* ctx = AppContext::tryGet()) {
        auto code = ctx->config().get("/translate/target", "").get<std::string>();
        if (!code.empty())
            return qs(code);
    }
    auto locale = QLocale::system();
    return qs(formats::translationLanguageFor(ss(locale.uiLanguages().value(0, locale.name()))));
}

void TranslationPopup::setTargetLanguage(const QString& code)
{
    if (auto* ctx = AppContext::tryGet()) {
        ctx->config().set("/translate/target", ss(code));
        try {
            ctx->config().save();
        } catch (const std::exception&) {
        }
    }
}

bool TranslationPopup::translateAtOnce()
{
    auto* ctx = AppContext::tryGet();
    return ctx && ctx->config().get("/translate/auto", false).get<bool>();
}

void TranslationPopup::fillLanguages(QComboBox* box, const QString& current)
{
    QSignalBlocker block(box);
    box->clear();
    for (auto& l : formats::translationLanguages())
        box->addItem(QString::fromUtf8(l.name), QString::fromUtf8(l.code));
    int i = box->findData(current);
    if (i < 0 && !current.isEmpty()) {
        box->addItem(current, current);  // (a language not listed, set by hand)
        i = box->count() - 1;
    }
    box->setCurrentIndex(std::max(i, 0));
}

QString TranslationPopup::cleanText(const QString& text, bool* cut)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (!isLineBreak(text[i])) {
            out += text[i].isSpace() ? QChar(' ') : text[i];
            continue;
        }
        int j = i + 1;
        while (j < text.size() && text[j].isSpace())
            ++j;
        QChar next = j < text.size() ? text[j] : QChar();
        while (out.endsWith(' '))
            out.chop(1);
        QChar prev = out.isEmpty() ? QChar() : out.back();
        if (prev == '-' && out.size() >= 2 && out[out.size() - 2].isLetter()) {
            if (next.isLower())
                out.chop(1);  // a word broken at the line's end; ("Self-Attention": a hyphen of its own)
        } else if (!out.isEmpty() && !(isCjk(prev) && isCjk(next)))
            out += ' ';  // (Chinese, Japanese: the lines run on without one)
        i = j - 1;
    }
    out = out.simplified();
    bool longer = out.size() > kMaxCharacters;
    if (longer) {
        int end = kMaxCharacters;
        int space = out.lastIndexOf(' ', end);
        if (space > end - 200)
            end = space;  // (not in a word)
        if (end > 0 && out[end - 1].isHighSurrogate())
            --end;
        out.truncate(end);
    }
    if (cut)
        *cut = longer;
    return out;
}

TranslationPopup::TranslationPopup(QWidget* parent) : QFrame(parent)
{
    setObjectName("translationPopup");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
    setCursor(Qt::ArrowCursor);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 4, 4, 6);
    v->setSpacing(2);
    auto* head = head_ = new QHBoxLayout;
    head->setSpacing(4);
    from_ = new QLabel(this);
    from_->setObjectName("translationFrom");
    head->addWidget(from_);
    to_ = new QComboBox(this);
    to_->setObjectName("translationTarget");
    to_->setToolTip(tr("The language to translate into"));
    to_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    to_->setFocusPolicy(Qt::NoFocus);
    head->addWidget(to_);
    head->addStretch(1);
    auto button = [this, head](const char* name, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(icon(name));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        head->addWidget(b);
        return b;
    };
    copy_ = button("copy", tr("Copy the translation"));
    copy_->setObjectName("translationCopy");
    open_ = button("open", tr("Open in Google Translate"));
    open_->setObjectName("translationOpen");
    auto* close = button("close", tr("Close (Esc)"));
    v->addLayout(head);
    body_ = new QTextBrowser(this);
    body_->setObjectName("translationText");
    body_->setFrameShape(QFrame::NoFrame);
    body_->setOpenLinks(false);
    body_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_->document()->setDocumentMargin(2);
    body_->viewport()->setAutoFillBackground(false);
    v->addWidget(body_);

    connect(to_, &QComboBox::currentIndexChanged, this, [this] {
        setTargetLanguage(to_->currentData().toString());
        if (!text_.isEmpty())
            request();
    });
    connect(copy_, &QToolButton::clicked, this, [this] {
        if (!translation_.isEmpty())
            QApplication::clipboard()->setText(translation_);
    });
    connect(open_, &QToolButton::clicked, this, [this] {
        auto url = formats::googleTranslatePage(ss(text_), ss(to_->currentData().toString()));
        QDesktopServices::openUrl(QUrl::fromEncoded(QByteArray::fromStdString(url)));
    });
    connect(close, &QToolButton::clicked, this, &TranslationPopup::cancel);
    hide();
}

void TranslationPopup::translate(const QString& text)
{
    text_ = cleanText(text, &cut_);
    if (text_.isEmpty())
        return;
    fillLanguages(to_, targetLanguage());
    request();
}

void TranslationPopup::cancel()
{
    ++generation_;
    busy_ = false;
    if (isAncestorOf(QApplication::focusWidget())) {  // (its text clicked): the keyboard back to the page
        QWidget* w = parentWidget();
        while (w && w->focusPolicy() == Qt::NoFocus)
            w = w->parentWidget();
        if (w)
            w->setFocus();
    }
    hide();
}

void TranslationPopup::request()
{
    auto generation = ++generation_;
    busy_ = true;
    translation_.clear();
    error_.clear();
    copy_->setEnabled(false);
    from_->setText(QStringLiteral("…"));
    showBody(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(dim(this), tr("Translating…")));
    QPointer<TranslationPopup> self(this);
    auto target = ss(to_->currentData().toString());
    std::thread([self, generation, text = ss(text_), target, fn = translator()] {
        formats::Translation t;
        bool intoEnglish = false;
        QString error;
        try {
            t = fn(text, target);
            // already in the language asked for: into English then
            if (formats::sameLanguage(t.source, target) && !formats::sameLanguage(target, "en")) {
                auto source = t.source;
                t = fn(text, "en");
                t.source = source;
                intoEnglish = true;
            }
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        auto* app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, generation, t, intoEnglish, error] {
                if (!self || self->generation_ != generation)
                    return;  // (closed, or something else asked since)
                self->busy_ = false;
                if (error.isEmpty())
                    self->showResult(t, intoEnglish);
                else
                    self->showError(error);
            },
            Qt::QueuedConnection);
    }).detach();
}

void TranslationPopup::showResult(const formats::Translation& t, bool intoEnglish)
{
    translation_ = qs(t.text);
    copy_->setEnabled(true);
    from_->setText(t.source.empty() ? QString() : qs(formats::languageName(t.source)) + QStringLiteral("  →"));
    QString html = QStringLiteral("<p style=\"margin:0\">%1</p>").arg(translation_.toHtmlEscaped());
    // how a word or two is said
    if (!t.phonetic.empty() && text_.size() <= 60 && text_.count(' ') <= 2)
        html += QStringLiteral("<p style=\"margin:4px 0 0 0; color:%1\">/%2/</p>")
                    .arg(dim(this), qs(t.phonetic).toHtmlEscaped());
    if (!t.dictionary.empty()) {
        html += QStringLiteral("<table style=\"margin-top:4px\" cellspacing=\"0\" cellpadding=\"1\">");
        for (auto& m : t.dictionary) {
            QStringList terms;
            for (auto& term : m.terms)
                terms << qs(term).toHtmlEscaped();
            html += QStringLiteral("<tr><td style=\"color:%1; padding-right:8px\">%2</td><td>%3</td></tr>")
                        .arg(dim(this), qs(m.partOfSpeech).toHtmlEscaped(), terms.join(QStringLiteral(", ")));
        }
        html += QStringLiteral("</table>");
    }
    QStringList notes;
    if (intoEnglish)
        notes << tr("It is in %1 already: into English.").arg(qs(formats::languageName(t.source)));
    if (cut_)
        notes << tr("Only the first %1 characters are translated.").arg(kMaxCharacters);
    if (!notes.isEmpty())
        html += QStringLiteral("<p style=\"margin:4px 0 0 0; color:%1\">%2</p>").arg(dim(this), notes.join(' '));
    showBody(html);
}

void TranslationPopup::showError(const QString& message)
{
    error_ = message;
    from_->clear();
    showBody(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(dim(this), message.toHtmlEscaped()));
}

// As wide as its longest line (wrapped at kMaxWidth) and as tall as its text, up to a
// part of the page; more scrolls.
void TranslationPopup::showBody(const QString& html)
{
    body_->setHtml(html);
    auto* area = parentWidget();
    int margins = layout()->contentsMargins().left() + layout()->contentsMargins().right() + 2 * frameWidth();
    head_->invalidate();  // (the language's name and the list changed: measured afresh)
    int head = head_->sizeHint().width();
    int maxBody = std::max(160, std::min(kMaxWidth, (area ? area->width() : kMaxWidth) - 16) - margins);
    int most = std::max(fontMetrics().height() * 3, std::min(kMaxHeight, (area ? area->height() : kMaxHeight) * 2 / 5));
    std::unique_ptr<QTextDocument> probe(body_->document()->clone());
    probe->setDefaultFont(body_->document()->defaultFont());
    probe->setDocumentMargin(body_->document()->documentMargin());
    probe->setTextWidth(maxBody);
    int width = std::clamp(int(std::ceil(probe->idealWidth())) + 1, std::min(head, maxBody), maxBody);
    probe->setTextWidth(width);
    int height = int(std::ceil(probe->size().height()));
    if (height > most) {  // the scroll bar takes some of the width
        probe->setTextWidth(width - body_->verticalScrollBar()->sizeHint().width());
        height = int(std::ceil(probe->size().height()));
    }
    body_->setVerticalScrollBarPolicy(height > most ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    body_->setFixedSize(width, std::min(height, most));
    adjustSize();
    show();
    raise();
    emit sizeChanged();
}

void TranslationPopup::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        cancel();
        return;
    }
    QFrame::keyPressEvent(e);
}

void TranslationPopup::mousePressEvent(QMouseEvent* e) { e->accept(); }
void TranslationPopup::mouseReleaseEvent(QMouseEvent* e) { e->accept(); }
void TranslationPopup::mouseDoubleClickEvent(QMouseEvent* e) { e->accept(); }
void TranslationPopup::contextMenuEvent(QContextMenuEvent* e) { e->accept(); }

} // namespace on::gui
