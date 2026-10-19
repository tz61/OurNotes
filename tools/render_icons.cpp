// Renders resources/icons/freepen.svg into the raster formats each platform needs:
//   hicolor/<N>x<N>/apps/freepen.png   (Linux icon theme)
//   freepen.ico                        (Windows executable resource)
//   freepen.icns                       (macOS application bundle)
// Sizes up to 24 px use freepen-small.svg (a simplified design) when it exists.
// Build with -DFREEPEN_BUILD_TOOLS=ON, then: render_icons resources/icons

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <map>

static bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        return false;
    }
    return true;
}

static QByteArray renderPng(QSvgRenderer& svg, int size)
{
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    svg.render(&p);
    p.end();
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return png;
}

static void put32be(QByteArray& b, quint32 v)
{
    for (int s = 24; s >= 0; s -= 8)
        b.append(char((v >> s) & 0xff));
}

static void put16le(QByteArray& b, quint16 v)
{
    b.append(char(v & 0xff));
    b.append(char(v >> 8));
}

static void put32le(QByteArray& b, quint32 v)
{
    for (int s = 0; s <= 24; s += 8)
        b.append(char((v >> s) & 0xff));
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: render_icons ICON_DIR [SVG] (default SVG: ICON_DIR/freepen.svg)\n");
        return 1;
    }
    QDir dir(QString::fromLocal8Bit(argv[1]));
    QString source = argc > 2 ? QString::fromLocal8Bit(argv[2]) : dir.filePath("freepen.svg");
    QSvgRenderer svg(source);
    if (!svg.isValid()) {
        std::fprintf(stderr, "cannot load %s\n", qPrintable(source));
        return 1;
    }
    QSvgRenderer small(dir.filePath("freepen-small.svg"));
    std::map<int, QByteArray> png;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256, 512, 1024})
        png[size] = renderPng(size <= 24 && small.isValid() ? small : svg, size);

    for (int size : {16, 22, 24, 32, 48, 64, 128, 256, 512}) {
        QString sub = QString("hicolor/%1x%1/apps").arg(size);
        dir.mkpath(sub);
        if (!writeFile(dir.filePath(sub + "/freepen.png"), png[size]))
            return 1;
    }

    // ICO with PNG-compressed entries (supported since Windows Vista)
    const int icoSizes[] = {16, 24, 32, 48, 64, 128, 256};
    QByteArray ico;
    put16le(ico, 0);
    put16le(ico, 1);
    put16le(ico, (quint16)std::size(icoSizes));
    quint32 offset = 6 + 16 * (quint32)std::size(icoSizes);
    for (int s : icoSizes) {
        ico.append(char(s >= 256 ? 0 : s));
        ico.append(char(s >= 256 ? 0 : s));
        ico.append(char(0));
        ico.append(char(0));
        put16le(ico, 1);
        put16le(ico, 32);
        put32le(ico, (quint32)png[s].size());
        put32le(ico, offset);
        offset += (quint32)png[s].size();
    }
    for (int s : icoSizes)
        ico.append(png[s]);
    if (!writeFile(dir.filePath("freepen.ico"), ico))
        return 1;

    // ICNS with PNG chunks
    const std::pair<const char*, int> icnsTypes[] = {{"icp4", 16},  {"icp5", 32},  {"icp6", 64},   {"ic07", 128},
                                                      {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}, {"ic11", 32},
                                                      {"ic12", 64},  {"ic13", 256}, {"ic14", 512}};
    QByteArray body;
    for (auto& [type, size] : icnsTypes) {
        body.append(type, 4);
        put32be(body, (quint32)png[size].size() + 8);
        body.append(png[size]);
    }
    QByteArray icns("icns", 4);
    put32be(icns, (quint32)body.size() + 8);
    icns.append(body);
    if (!writeFile(dir.filePath("freepen.icns"), icns))
        return 1;

    // contact sheet for reviewing the design at small sizes
    if (qEnvironmentVariableIsSet("FREEPEN_ICON_PREVIEW")) {
        QImage sheet(16 + 24 + 32 + 48 + 64 + 128 + 256 + 8 * 12, 256 + 24, QImage::Format_ARGB32);
        sheet.fill(QColor("#f2f2f2"));
        QPainter p(&sheet);
        int x = 12;
        for (int s : {16, 24, 32, 48, 64, 128, 256}) {
            p.drawImage(x, 12 + (256 - s) / 2, QImage::fromData(png[s]));
            x += s + 12;
        }
        p.end();
        sheet.save(qEnvironmentVariable("FREEPEN_ICON_PREVIEW"));
    }
    std::printf("wrote PNGs, freepen.ico and freepen.icns to %s\n", qPrintable(dir.absolutePath()));
    return 0;
}
