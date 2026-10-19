#include "gui/reader/annotation_sidebar.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/schema.h"
#include "gui/common.h"
#include "gui/icons.h"
#include "gui/item_pane.h"

namespace fp::gui {

namespace {

QString typeName(const QString& type)
{
    if (type == "highlight")
        return QObject::tr("Highlight");
    if (type == "underline")
        return QObject::tr("Underline");
    if (type == "note")
        return QObject::tr("Note");
    if (type == "text")
        return QObject::tr("Text");
    if (type == "image")
        return QObject::tr("Image");
    if (type == "ink")
        return QObject::tr("Ink");
    return type;
}

} // namespace

// --- FitImageView ----------------------------------------------------------------------------

void FitImageView::setImage(const QImage& image)
{
    image_ = image;
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    updateGeometry();
    update();
}

int FitImageView::heightForWidth(int width) const
{
    if (image_.isNull() || image_.width() == 0)
        return 0;
    qreal w = std::min<qreal>(width, image_.width() / image_.devicePixelRatio());
    return qRound(w * image_.height() / image_.width());
}

QSize FitImageView::sizeHint() const
{
    if (image_.isNull())
        return {0, 0};
    int w = qRound(image_.width() / image_.devicePixelRatio());
    return {w, heightForWidth(w)};
}

void FitImageView::paintEvent(QPaintEvent*)
{
    if (image_.isNull())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    qreal w = std::min<qreal>(width(), image_.width() / image_.devicePixelRatio());
    qreal h = w * image_.height() / image_.width();
    p.drawImage(QRectF((width() - w) / 2, 0, w, h), image_);
}

// --- AnnotationCard ----------------------------------------------------------------------

AnnotationCard::AnnotationCard(const Json& data, bool editable, QWidget* parent)
    : QFrame(parent), data_(data), editable_(editable)
{
    setObjectName("annotationCard");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 4, 6, 6);
    v->setSpacing(3);
    auto* head = new QHBoxLayout;
    header_ = new QLabel(this);
    menu_ = new QToolButton(this);
    menu_->setIcon(icon("menu"));
    menu_->setAutoRaise(true);
    menu_->setPopupMode(QToolButton::InstantPopup);
    head->addWidget(header_, 1);
    head->addWidget(menu_);
    v->addLayout(head);
    text_ = new QLabel(this);
    text_->setWordWrap(true);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(text_);
    image_ = new FitImageView(this);
    v->addWidget(image_);
    comment_ = new AutoTextEdit(false, this);
    comment_->setPlaceholderText(tr("Add comment"));
    v->addWidget(comment_);
    tags_ = new QLabel(this);
    tags_->setWordWrap(true);
    v->addWidget(tags_);

    auto* menu = new QMenu(menu_);
    auto* colors = menu->addMenu(tr("Colour"));
    for (auto& c : schema::kAnnotationColors)
        colors->addAction(colorIcon(c.hex), tr(c.name), this, [this, hex = std::string(c.hex)] {
            Json d = data_;
            d["annotationColor"] = hex;
            emit changed(d);
        });
    menu->addAction(icon("copy"), tr("Copy Text"), this, [this] {
        auto text = jqs(data_, "annotationText");
        QApplication::clipboard()->setText(text.isEmpty() ? jqs(data_, "annotationComment") : text);
    });
    menu->addAction(icon("new-note"), tr("Add to Note"), this, [this] { emit addToNote(data_); });
    menu->addSeparator();
    menu->addAction(icon("trash"), tr("Delete"), this, [this] { emit deleted(jqs(data_, "key")); });
    menu_->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, colors, menu] {
        bool can = editable_ && !jbool(data_, "annotationIsExternal");
        colors->setEnabled(can);
        menu->actions().last()->setEnabled(can);
    });
    connect(comment_, &AutoTextEdit::editingFinished, this, [this] {
        auto text = comment_->toPlainText();
        if (text == jqs(data_, "annotationComment"))
            return;
        Json d = data_;
        d["annotationComment"] = ss(text);
        emit changed(d);
    });
    refresh();
}

void AnnotationCard::update(const Json& data, bool editable)
{
    bool imageChanged = data.value("annotationPosition", Json()) != data_.value("annotationPosition", Json());
    data_ = data;
    editable_ = editable;
    if (imageChanged)
        image_->setImage({});
    refresh();
}

bool AnnotationCard::isEditing() const { return comment_->hasFocus(); }

void AnnotationCard::focusComment()
{
    comment_->setFocus();
    comment_->moveCursor(QTextCursor::End);
}

void AnnotationCard::setSelected(bool selected)
{
    if (selected == selected_)
        return;
    selected_ = selected;
    refresh();
}

void AnnotationCard::refresh()
{
    auto type = jqs(data_, "annotationType");
    QColor color(jqs(data_, "annotationColor"));
    bool external = jbool(data_, "annotationIsExternal");
    QString page = jqs(data_, "annotationPageLabel");
    header_->setText(QString("<span style=\"color:%1\">■</span> %2 &nbsp;<small>%3</small>%4")
                         .arg(color.name(), typeName(type), page.isEmpty() ? QString() : tr("Page %1").arg(page),
                              external ? " <small>(" + tr("in file") + ")</small>" : QString()));
    auto text = jqs(data_, "annotationText");
    text_->setVisible(!text.isEmpty());
    text_->setText("<i>" + text.toHtmlEscaped() + "</i>");
    if (type == "image" && !image_->hasImage() && imageProvider) {
        auto img = imageProvider(data_);
        if (!img.isNull()) {
            img.setDevicePixelRatio(devicePixelRatioF());
            image_->setImage(img);
        }
    }
    image_->setVisible(type == "image");
    if (!comment_->hasFocus())
        comment_->setTextSilently(jqs(data_, "annotationComment"));
    comment_->setReadOnly(!editable_ || external);
    bool hasComment = !comment_->toPlainText().isEmpty();
    comment_->setVisible(hasComment || (selected_ && !comment_->isReadOnly()) || comment_->hasFocus());
    QStringList tags;
    for (auto& t : data_.value("tags", Json::array()))
        tags << "#" + jqs(t, "tag");
    tags_->setText(tags.join(" "));
    tags_->setVisible(!tags.isEmpty());
    QWidget::update();
}

// Painted rather than styled with a style sheet: style sheets on hundreds of cards are
// slow, and the colours here follow theme changes without any refresh.
void AnnotationCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor base = palette().color(QPalette::Base), hl = palette().color(QPalette::Highlight);
    QColor bg = selected_ ? QColor::fromRgbF(base.redF() * 0.75 + hl.redF() * 0.25, base.greenF() * 0.75 + hl.greenF() * 0.25,
                                             base.blueF() * 0.75 + hl.blueF() * 0.25)
                          : base;
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.setBrush(bg);
    p.drawRoundedRect(r, 3, 3);
    // the annotation's colour along the left edge
    QPainterPath edge;
    edge.addRoundedRect(QRectF(0, 0, 8, height()), 3, 3);
    p.setClipRect(QRectF(0, 0, 4, height()));
    p.fillPath(edge, QColor(jqs(data_, "annotationColor")));
}

void AnnotationCard::mousePressEvent(QMouseEvent* e)
{
    emit activated(jqs(data_, "key"));
    QFrame::mousePressEvent(e);
}

// --- AnnotationSidebar ---------------------------------------------------------------------------

AnnotationSidebar::AnnotationSidebar(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search annotations"));
    search_->setClearButtonEnabled(true);
    v->addWidget(search_);
    colorBar_ = new QWidget(this);
    auto* bar = new QHBoxLayout(colorBar_);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    for (auto& c : schema::kAnnotationColors) {
        auto* b = new QToolButton(colorBar_);
        b->setIcon(colorIcon(c.hex, 12));
        b->setToolTip(tr("Show only %1").arg(c.name));
        b->setCheckable(true);
        b->setAutoRaise(true);
        connect(b, &QToolButton::toggled, this, [this, b, hex = QString(c.hex)](bool on) {
            for (auto* other : colorBar_->findChildren<QToolButton*>())
                if (other != b && on)
                    other->setChecked(false);
            colorFilter_ = on ? hex : QString();
            applyFilter();
        });
        bar->addWidget(b);
    }
    bar->addStretch();
    v->addWidget(colorBar_);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    container_ = new QWidget;
    list_ = new QVBoxLayout(container_);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(6);
    empty_ = new QLabel(tr("No annotations yet.\n\nSelect text to highlight it, or pick a tool in the toolbar."),
                        container_);
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignCenter);
    list_->addWidget(empty_);
    list_->addStretch();
    scroll_->setWidget(container_);
    v->addWidget(scroll_, 1);
    connect(search_, &QLineEdit::textChanged, this, &AnnotationSidebar::applyFilter);
}

void AnnotationSidebar::setAnnotations(const std::vector<Json>& annotations, bool editable)
{
    QSet<QString> present;
    int index = 0;
    for (auto& a : annotations) {
        auto key = jqs(a, "key");
        present.insert(key);
        bool canEdit = editable && !jbool(a, "annotationIsExternal");
        AnnotationCard* card = cards_.value(key);
        if (!card) {
            card = new AnnotationCard(a, canEdit, container_);
            card->imageProvider = imageProvider;
            connect(card, &AnnotationCard::activated, this, &AnnotationSidebar::activated);
            connect(card, &AnnotationCard::changed, this, &AnnotationSidebar::changed);
            connect(card, &AnnotationCard::deleted, this, &AnnotationSidebar::deleted);
            connect(card, &AnnotationCard::addToNote, this, &AnnotationSidebar::addToNote);
            card->update(a, canEdit);
            cards_.insert(key, card);
        } else if (card->data() != a) {
            card->update(a, canEdit);
        }
        card->setSelected(key == selected_);
        // keep layout order = annotation order (by sort index)
        int current = list_->indexOf(card);
        if (current != index + 1) {
            list_->removeWidget(card);
            list_->insertWidget(index + 1, card);
        }
        ++index;
    }
    for (auto it = cards_.begin(); it != cards_.end();) {
        if (!present.contains(it.key())) {
            it.value()->deleteLater();
            it = cards_.erase(it);
        } else {
            ++it;
        }
    }
    empty_->setVisible(annotations.empty());
    applyFilter();
}

void AnnotationSidebar::setSelected(const QString& key, bool scroll)
{
    if (auto* old = cards_.value(selected_))
        old->setSelected(false);
    selected_ = key;
    if (auto* card = cards_.value(key)) {
        card->setSelected(true);
        if (scroll)
            scroll_->ensureWidgetVisible(card, 0, 40);
    }
}

void AnnotationSidebar::focusComment(const QString& key)
{
    if (auto* card = cards_.value(key)) {
        scroll_->ensureWidgetVisible(card);
        card->focusComment();
    }
}

void AnnotationSidebar::applyFilter()
{
    auto q = normalize(ss(search_->text()));
    for (auto* card : cards_) {
        auto& d = card->data();
        bool ok = colorFilter_.isEmpty() || jqs(d, "annotationColor").compare(colorFilter_, Qt::CaseInsensitive) == 0;
        if (ok && !q.empty()) {
            std::string hay = normalize(jstr(d, "annotationText") + " " + jstr(d, "annotationComment"));
            for (auto& t : d.value("tags", Json::array()))
                hay += " " + normalize(jstr(t, "tag"));
            ok = hay.find(q) != std::string::npos;
        }
        card->setVisible(ok);
    }
}

} // namespace fp::gui
