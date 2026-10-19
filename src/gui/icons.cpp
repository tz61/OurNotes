#include "gui/icons.h"

#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPixmapCache>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSvgRenderer>

namespace fp::gui {

namespace {

// 24x24 viewBox; "CUR" is replaced with the palette's text colour.
const QHash<QString, QString>& sources()
{
    static const QHash<QString, QString> s = {
        // --- item types -------------------------------------------------------------
        {"type-article", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#3b82c4" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#3b82c4" stroke-width="1.5"/><path d="M8 11h9M8 14h9M8 17h6" stroke="#3b82c4" stroke-width="1.5"/>)"},
        {"type-book", R"(<path d="M5 3h12a2 2 0 0 1 2 2v16H7a2 2 0 0 1-2-2z" fill="#c98a3d"/><path d="M5 19a2 2 0 0 1 2-2h12" fill="none" stroke="#7a4f1d" stroke-width="1.5"/><path d="M9 7h7" stroke="#fff" stroke-width="1.5"/>)"},
        {"type-booksection", R"(<path d="M5 3h12a2 2 0 0 1 2 2v16H7a2 2 0 0 1-2-2z" fill="#c98a3d"/><path d="M5 19a2 2 0 0 1 2-2h12" fill="none" stroke="#7a4f1d" stroke-width="1.5"/><path d="M12 3v8l2-1.5 2 1.5V3" fill="#d94040"/>)"},
        {"type-conference", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#3a9a5b" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#3a9a5b" stroke-width="1.5"/><circle cx="12.5" cy="13" r="3" fill="#3a9a5b"/><path d="M8 20a4.5 4 0 0 1 9 0" fill="#3a9a5b"/>)"},
        {"type-preprint", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#d9822b" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#d9822b" stroke-width="1.5"/><path d="M8 11h9M8 14h9M8 17h6" stroke="#d9822b" stroke-width="1.5" stroke-dasharray="2 1.5"/>)"},
        {"type-thesis", R"(<path d="M2 9l10-5 10 5-10 5z" fill="#4a5a8c"/><path d="M6 11v5c3 2.5 9 2.5 12 0v-5l-6 3z" fill="#6f80b5"/><path d="M21 9v6" stroke="#4a5a8c" stroke-width="1.5"/>)"},
        {"type-report", R"(<path d="M5 2h14v20H5z" fill="#fff" stroke="#6b6bb5" stroke-width="1.5"/><path d="M8 18v-4M12 18v-8M16 18v-6" stroke="#6b6bb5" stroke-width="2"/>)"},
        {"type-webpage", R"(<circle cx="12" cy="12" r="9" fill="#e8f2fb" stroke="#2f7fc1" stroke-width="1.5"/><path d="M3 12h18M12 3c-3 3-3 15 0 18M12 3c3 3 3 15 0 18" fill="none" stroke="#2f7fc1" stroke-width="1.3"/>)"},
        {"type-software", R"(<rect x="2.5" y="4" width="19" height="16" rx="2" fill="#2d3748"/><path d="M8 9l-3 3 3 3M16 9l3 3-3 3M13.5 8l-3 8" stroke="#9ae6b4" stroke-width="1.6" fill="none"/>)"},
        {"type-document", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#7a8794" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#7a8794" stroke-width="1.5"/><path d="M8 12h9M8 15h9M8 18h6" stroke="#7a8794" stroke-width="1.5"/>)"},
        {"type-dataset", R"(<ellipse cx="12" cy="5.5" rx="8" ry="3" fill="#7fb3d5" stroke="#2c6e9e" stroke-width="1.2"/><path d="M4 5.5v13c0 1.7 3.6 3 8 3s8-1.3 8-3v-13" fill="#a9cce3" stroke="#2c6e9e" stroke-width="1.2"/><path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" fill="none" stroke="#2c6e9e" stroke-width="1.2"/>)"},
        {"type-patent", R"(<path d="M5 2h14v20H5z" fill="#fff" stroke="#8c6d4a" stroke-width="1.5"/><circle cx="12" cy="10" r="4" fill="#e0b84a"/><path d="M10 13l-1.5 6 3.5-2 3.5 2-1.5-6" fill="#e0b84a"/>)"},
        {"type-media", R"(<rect x="3" y="5" width="18" height="14" rx="2" fill="#4a4a6a"/><path d="M10 9v6l5-3z" fill="#fff"/>)"},
        {"type-letter", R"(<rect x="3" y="5" width="18" height="14" rx="1.5" fill="#fff" stroke="#7a8794" stroke-width="1.5"/><path d="M3.5 6l8.5 7 8.5-7" fill="none" stroke="#7a8794" stroke-width="1.5"/>)"},
        {"type-note", R"(<path d="M4 3h16v12l-6 6H4z" fill="#ffd65c" stroke="#c9a227" stroke-width="1.3"/><path d="M14 21v-6h6" fill="#f2c230" stroke="#c9a227" stroke-width="1.3"/><path d="M7 8h10M7 11h10M7 14h5" stroke="#a07f13" stroke-width="1.3"/>)"},
        {"type-pdf", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#d63c3c" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#d63c3c" stroke-width="1.5"/><rect x="3" y="12" width="15" height="7" rx="1" fill="#d63c3c"/><text x="10.5" y="17.6" font-family="sans-serif" font-size="5.6" font-weight="bold" fill="#fff" text-anchor="middle">PDF</text>)"},
        {"type-epub", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#3a9a5b" stroke-width="1.5"/><rect x="3" y="12" width="17" height="7" rx="1" fill="#3a9a5b"/><text x="11.5" y="17.6" font-family="sans-serif" font-size="5.2" font-weight="bold" fill="#fff" text-anchor="middle">EPUB</text>)"},
        {"type-snapshot", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#2f7fc1" stroke-width="1.5"/><circle cx="12.5" cy="14" r="5" fill="#e8f2fb" stroke="#2f7fc1" stroke-width="1.2"/><path d="M7.5 14h10M12.5 9c-2 2-2 8 0 10M12.5 9c2 2 2 8 0 10" fill="none" stroke="#2f7fc1" stroke-width="1"/>)"},
        {"type-link", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1" fill="none" stroke="#2f7fc1" stroke-width="2" stroke-linecap="round"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1" fill="none" stroke="#2f7fc1" stroke-width="2" stroke-linecap="round"/>)"},
        {"type-image", R"(<rect x="3" y="4" width="18" height="16" rx="1.5" fill="#fff" stroke="#7a8794" stroke-width="1.5"/><circle cx="9" cy="9.5" r="2" fill="#e0b84a"/><path d="M4 19l5-6 4 4 3-3 5 5z" fill="#5fb236"/>)"},
        {"type-file", R"(<path d="M5 2h10l5 5v15H5z" fill="#fff" stroke="#7a8794" stroke-width="1.5"/><path d="M15 2v5h5" fill="none" stroke="#7a8794" stroke-width="1.5"/>)"},
        {"type-annotation", R"(<path d="M4 18l3-8 8-8 5 5-8 8z" fill="#ffd400" stroke="#b39200" stroke-width="1.2"/><path d="M4 18l3 3H3z" fill="#555"/>)"},
        // --- collections tree --------------------------------------------------------
        {"library", R"(<path d="M3 11l9-7 9 7v10H3z" fill="#5b8bd6"/><rect x="9.5" y="14" width="5" height="7" fill="#fff"/>)"},
        {"group", R"(<circle cx="8" cy="8" r="3.5" fill="#5b8bd6"/><circle cx="16.5" cy="9" r="3" fill="#8fb3e8"/><path d="M1.5 20c0-4 3-6.5 6.5-6.5s6.5 2.5 6.5 6.5z" fill="#5b8bd6"/><path d="M13 20c0-3 1.5-5.5 3.5-5.5s6 1.5 6 5.5z" fill="#8fb3e8"/>)"},
        {"folder", R"(<path d="M2 6a2 2 0 0 1 2-2h5l2 2.5h9a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z" fill="#e8b84a"/><path d="M2 9h20" stroke="#c9952a" stroke-width="1"/>)"},
        {"unfiled", R"(<path d="M2 6a2 2 0 0 1 2-2h5l2 2.5h9a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z" fill="#c9ccd1"/><text x="12" y="17.5" font-family="sans-serif" font-size="9" font-weight="bold" fill="#555" text-anchor="middle">?</text>)"},
        {"duplicates", R"(<path d="M8 2h9l4 4v12H8z" fill="#fff" stroke="#7a8794" stroke-width="1.4"/><path d="M3 6h9l4 4v12H3z" fill="#fff" stroke="#3b82c4" stroke-width="1.4"/>)"},
        {"trash", R"(<path d="M4 6h16M9 6V4h6v2" stroke="CUR" stroke-width="1.6" fill="none"/><path d="M6 6l1 15h10l1-15z" fill="none" stroke="CUR" stroke-width="1.6"/><path d="M10 10v8M14 10v8" stroke="CUR" stroke-width="1.4"/>)"},
        // --- actions (monochrome) ----------------------------------------------------------
        {"new-item", R"(<circle cx="12" cy="12" r="9" fill="#3a9a5b"/><path d="M12 7v10M7 12h10" stroke="#fff" stroke-width="2.2"/>)"},
        {"new-collection", R"(<path d="M2 6a2 2 0 0 1 2-2h5l2 2.5h9a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z" fill="#e8b84a"/><path d="M12 10v7M8.5 13.5h7" stroke="#fff" stroke-width="2"/>)"},
        {"new-note", R"(<path d="M4 3h16v12l-6 6H4z" fill="#ffd65c" stroke="#c9a227" stroke-width="1.3"/><path d="M12 7v8M8 11h8" stroke="#8a6d0b" stroke-width="2"/>)"},
        {"attach", R"(<path d="M16.5 7.5l-7 7a2 2 0 0 0 2.8 2.8l7.5-7.5a4 4 0 0 0-5.7-5.7L6.4 11.8a6 6 0 0 0 8.5 8.5l6.6-6.6" fill="none" stroke="CUR" stroke-width="1.7" stroke-linecap="round"/>)"},
        {"magic", R"(<path d="M4 20L16 8" stroke="CUR" stroke-width="2.2" stroke-linecap="round"/><path d="M17 2l1 2.5L20.5 5.5 18 6.5 17 9 16 6.5 13.5 5.5 16 4.5zM8 3l.6 1.4L10 5l-1.4.6L8 7l-.6-1.4L6 5l1.4-.6zM20 12l.6 1.4 1.4.6-1.4.6L20 16l-.6-1.4L18 14l1.4-.6z" fill="#e0a800"/>)"},
        {"sync", R"(<path d="M20 12a8 8 0 0 1-13.7 5.6M4 12a8 8 0 0 1 13.7-5.6" fill="none" stroke="CUR" stroke-width="1.8" stroke-linecap="round"/><path d="M18.5 2.5v4.5H14M5.5 21.5V17H10" fill="none" stroke="CUR" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"sync-error", R"(<path d="M20 12a8 8 0 0 1-13.7 5.6M4 12a8 8 0 0 1 13.7-5.6" fill="none" stroke="#d63c3c" stroke-width="1.8" stroke-linecap="round"/><path d="M12 8v5M12 15.5v.5" stroke="#d63c3c" stroke-width="2.2" stroke-linecap="round"/>)"},
        {"search", R"(<circle cx="10.5" cy="10.5" r="6" fill="none" stroke="CUR" stroke-width="1.8"/><path d="M15 15l5.5 5.5" stroke="CUR" stroke-width="2" stroke-linecap="round"/>)"},
        {"open", R"(<path d="M14 4h6v6M20 4l-9 9" fill="none" stroke="CUR" stroke-width="1.8" stroke-linecap="round"/><path d="M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5" fill="none" stroke="CUR" stroke-width="1.8"/>)"},
        {"tag", R"(<path d="M3 12V4a1 1 0 0 1 1-1h8l9 9-9 9z" fill="none" stroke="CUR" stroke-width="1.7" stroke-linejoin="round"/><circle cx="7.5" cy="7.5" r="1.6" fill="CUR"/>)"},
        {"remove", R"(<circle cx="12" cy="12" r="8" fill="none" stroke="CUR" stroke-width="1.5"/><path d="M8 12h8" stroke="CUR" stroke-width="1.8"/>)"},
        {"add", R"(<circle cx="12" cy="12" r="8" fill="none" stroke="CUR" stroke-width="1.5"/><path d="M12 8v8M8 12h8" stroke="CUR" stroke-width="1.8"/>)"},
        {"close", R"(<path d="M6 6l12 12M18 6L6 18" stroke="CUR" stroke-width="1.8" stroke-linecap="round"/>)"},
        {"swap", R"(<path d="M4 8h14l-3-3M20 16H6l3 3" fill="none" stroke="CUR" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"menu", R"(<circle cx="5" cy="12" r="1.8" fill="CUR"/><circle cx="12" cy="12" r="1.8" fill="CUR"/><circle cx="19" cy="12" r="1.8" fill="CUR"/>)"},
        {"copy", R"(<rect x="8" y="8" width="12" height="13" rx="1.5" fill="none" stroke="CUR" stroke-width="1.6"/><path d="M16 8V4.5A1.5 1.5 0 0 0 14.5 3h-9A1.5 1.5 0 0 0 4 4.5v11A1.5 1.5 0 0 0 5.5 17H8" fill="none" stroke="CUR" stroke-width="1.6"/>)"},
        // --- reader -------------------------------------------------------------------------
        {"sidebar", R"(<rect x="3" y="4" width="18" height="16" rx="2" fill="none" stroke="CUR" stroke-width="1.6"/><path d="M9 4v16" stroke="CUR" stroke-width="1.6"/><path d="M5 8h2M5 11h2M5 14h2" stroke="CUR" stroke-width="1.3"/>)"},
        {"context-pane", R"(<rect x="3" y="4" width="18" height="16" rx="2" fill="none" stroke="CUR" stroke-width="1.6"/><path d="M15 4v16" stroke="CUR" stroke-width="1.6"/>)"},
        {"zoom-in", R"(<circle cx="10.5" cy="10.5" r="6.5" fill="none" stroke="CUR" stroke-width="1.7"/><path d="M15.5 15.5l5 5M10.5 7.5v6M7.5 10.5h6" stroke="CUR" stroke-width="1.8" stroke-linecap="round"/>)"},
        {"zoom-out", R"(<circle cx="10.5" cy="10.5" r="6.5" fill="none" stroke="CUR" stroke-width="1.7"/><path d="M15.5 15.5l5 5M7.5 10.5h6" stroke="CUR" stroke-width="1.8" stroke-linecap="round"/>)"},
        {"fit-width", R"(<path d="M3 4v16M21 4v16" stroke="CUR" stroke-width="1.6"/><path d="M6 12h12M9 9l-3 3 3 3M15 9l3 3-3 3" fill="none" stroke="CUR" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"fit-page", R"(<rect x="6" y="3" width="12" height="18" rx="1" fill="none" stroke="CUR" stroke-width="1.6"/><path d="M9 8h6M9 12h6M9 16h4" stroke="CUR" stroke-width="1.3"/>)"},
        {"prev", R"(<path d="M15 5l-7 7 7 7" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"next", R"(<path d="M9 5l7 7-7 7" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"up", R"(<path d="M5 15l7-7 7 7" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"down", R"(<path d="M5 9l7 7 7-7" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>)"},
        {"tool-select", R"(<path d="M6 3l12 9-5.5 1 3.5 7-2.5 1-3.5-7L6 18z" fill="CUR"/>)"},
        {"tool-highlight", R"(<path d="M14.5 3.5l6 6-8 8H6.5v-6z" fill="#ffd400" stroke="CUR" stroke-width="1.4" stroke-linejoin="round"/><path d="M6.5 17.5L3 21h6" fill="CUR"/><path d="M2 22h20" stroke="#ffd400" stroke-width="2"/>)"},
        {"tool-underline", R"(<path d="M7 4v7a5 5 0 0 0 10 0V4" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round"/><path d="M5 20h14" stroke="#ff6666" stroke-width="2.2" stroke-linecap="round"/>)"},
        {"tool-note", R"(<path d="M4 3h16v12l-6 6H4z" fill="#ffd65c" stroke="CUR" stroke-width="1.3"/><path d="M14 21v-6h6" fill="none" stroke="CUR" stroke-width="1.3"/>)"},
        {"tool-text", R"(<path d="M5 5h14M12 5v15M9 20h6" stroke="CUR" stroke-width="2" stroke-linecap="round"/>)"},
        {"tool-area", R"(<rect x="3.5" y="3.5" width="17" height="17" fill="none" stroke="CUR" stroke-width="1.6" stroke-dasharray="3 2"/><path d="M8 16l3-4 2 2.5 2-2 2 3.5z" fill="CUR"/>)"},
        {"tool-ink", R"(<path d="M3 19c3-1 4-6 7-6s2 5 5 5 4-5 6-7" fill="none" stroke="CUR" stroke-width="2" stroke-linecap="round"/>)"},
        {"tool-eraser", R"(<path d="M14 3l7 7-10 10H6l-3-3z" fill="none" stroke="CUR" stroke-width="1.6" stroke-linejoin="round"/><path d="M9 8l7 7" stroke="CUR" stroke-width="1.6"/><path d="M11 20h10" stroke="CUR" stroke-width="1.6"/>)"},
        {"thumbnails", R"(<rect x="4" y="3" width="7" height="8" fill="none" stroke="CUR" stroke-width="1.5"/><rect x="13" y="3" width="7" height="8" fill="none" stroke="CUR" stroke-width="1.5"/><rect x="4" y="13" width="7" height="8" fill="none" stroke="CUR" stroke-width="1.5"/><rect x="13" y="13" width="7" height="8" fill="none" stroke="CUR" stroke-width="1.5"/>)"},
        {"outline", R"(<path d="M4 5h2M9 5h11M6 10h2M11 10h9M6 15h2M11 15h9M4 20h2M9 20h11" stroke="CUR" stroke-width="1.7"/>)"},
        {"annotations", R"(<path d="M4 4h16v12H10l-5 4v-4H4z" fill="none" stroke="CUR" stroke-width="1.6" stroke-linejoin="round"/><path d="M8 8h8M8 11.5h5" stroke="CUR" stroke-width="1.5"/>)"},
        {"export", R"(<path d="M12 3v12M7 8l5-5 5 5" fill="none" stroke="CUR" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/><path d="M4 14v6h16v-6" fill="none" stroke="CUR" stroke-width="1.8"/>)"},
    };
    return s;
}

QColor textColor() { return QApplication::palette().color(QPalette::WindowText); }

// Renders the SVG at exactly the size requested, so icons stay sharp at any
// interface scale. "CUR" in the source is replaced by the current text colour when
// painting, so monochrome icons follow light/dark theme switches.
class SvgIconEngine : public QIconEngine {
public:
    explicit SvgIconEngine(QString body) : body_(std::move(body)), id_(qHash(body_)) {}

    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        QSvgRenderer renderer(svg());
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (mode == QIcon::Disabled)
            p->setOpacity(0.35);
        renderer.render(p, rect);
        p->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QString cacheKey = QString("fp-icon-%1-%2x%3-%4-%5")
                               .arg(id_)
                               .arg(size.width())
                               .arg(size.height())
                               .arg(int(mode))
                               .arg(textColor().rgba());
        QPixmap pm;
        if (QPixmapCache::find(cacheKey, &pm))
            return pm;
        pm = QPixmap(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        p.end();
        QPixmapCache::insert(cacheKey, pm);
        return pm;
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm = pixmap(QSize(qRound(size.width() * scale), qRound(size.height() * scale)), mode, state);
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QIconEngine* clone() const override { return new SvgIconEngine(body_); }
    QString key() const override { return QStringLiteral("freepen-svg"); }

private:
    QByteArray svg() const
    {
        return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">%1</svg>")
            .arg(QString(body_).replace("CUR", textColor().name()))
            .toUtf8();
    }

    QString body_;
    size_t id_;
};

QIcon render(const QString& svgBody) { return QIcon(new SvgIconEngine(svgBody)); }

} // namespace

QIcon icon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end())
        return *it;
    auto src = sources().value(name);
    QIcon ic = src.isEmpty() ? QIcon() : render(src);
    cache.insert(name, ic);
    return ic;
}

QIcon itemTypeIcon(const QString& t, const QString& contentType, const QString& linkMode)
{
    if (t == "attachment") {
        if (linkMode == "linked_url")
            return icon("type-link");
        if (contentType == "application/pdf")
            return icon("type-pdf");
        if (contentType == "application/epub+zip")
            return icon("type-epub");
        if (contentType == "text/html" || linkMode == "imported_url")
            return icon("type-snapshot");
        if (contentType.startsWith("image/"))
            return icon("type-image");
        return icon("type-file");
    }
    static const QHash<QString, QString> map = {
        {"note", "type-note"},
        {"annotation", "type-annotation"},
        {"journalArticle", "type-article"},
        {"magazineArticle", "type-article"},
        {"newspaperArticle", "type-article"},
        {"encyclopediaArticle", "type-article"},
        {"dictionaryEntry", "type-article"},
        {"book", "type-book"},
        {"bookSection", "type-booksection"},
        {"conferencePaper", "type-conference"},
        {"presentation", "type-conference"},
        {"preprint", "type-preprint"},
        {"manuscript", "type-preprint"},
        {"thesis", "type-thesis"},
        {"report", "type-report"},
        {"standard", "type-report"},
        {"webpage", "type-webpage"},
        {"blogPost", "type-webpage"},
        {"forumPost", "type-webpage"},
        {"computerProgram", "type-software"},
        {"dataset", "type-dataset"},
        {"patent", "type-patent"},
        {"film", "type-media"},
        {"videoRecording", "type-media"},
        {"tvBroadcast", "type-media"},
        {"audioRecording", "type-media"},
        {"podcast", "type-media"},
        {"radioBroadcast", "type-media"},
        {"letter", "type-letter"},
        {"email", "type-letter"},
        {"artwork", "type-image"},
        {"map", "type-image"},
    };
    return icon(map.value(t, "type-document"));
}

QIcon colorIcon(const QString& color, int size, bool round)
{
    QPixmap pm(size * 3, size * 3);
    pm.setDevicePixelRatio(3);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(color));
    p.setPen(QColor(color).darker(130));
    QRectF r(1, 1, size - 2, size - 2);
    if (round)
        p.drawEllipse(r);
    else
        p.drawRoundedRect(r, 2, 2);
    return QIcon(pm);
}

} // namespace fp::gui
