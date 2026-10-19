// freePen desktop application.

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>

#include "core/config.h"
#include "core/library.h"
#include "core/zotero_importer.h"
#include "freepen/version.h"
#include "gui/app_context.h"
#include "gui/common.h"
#include "gui/main_window.h"
#include "gui/theme.h"
#include "gui/ui_scale.h"

using namespace fp;
using namespace fp::gui;

// The --config option has to be known before the QApplication exists (for the scale).
static fs::path configPathFromArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc)
            return u8path(argv[i + 1]);
        if (startsWith(a, "--config="))
            return u8path(a.substr(9));
    }
    return {};
}

int main(int argc, char** argv)
{
    std::unique_ptr<Config> config;
    try {
        config = std::make_unique<Config>(configPathFromArgs(argc, argv));
    } catch (const std::exception&) {
        config = std::make_unique<Config>(fs::path());
    }
    applyInterfaceScale(*config);

    QApplication app(argc, argv);
    QApplication::setApplicationName("freePen");
    QApplication::setOrganizationName("freePen");
    QApplication::setApplicationVersion(FREEPEN_VERSION);
    QApplication::setDesktopFileName("freepen");
    QApplication::setWindowIcon(QIcon(":/freepen.svg"));
    applyAppearance(appearanceFromConfig(*config));

    QCommandLineParser parser;
    parser.setApplicationDescription("freePen reference manager");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption dataDirOpt("data-dir", "Use DIR as the data directory.", "DIR");
    QCommandLineOption configOpt("config", "Use FILE as the configuration file.", "FILE");
    // developer options: render the window to an image (used for UI tests and docs)
    QCommandLineOption screenshotOpt("screenshot", "Save a screenshot to FILE and exit.", "FILE");
    QCommandLineOption openOpt("open", "Open the attachment with KEY in a reader tab.", "KEY");
    QCommandLineOption selectOpt("select", "Select the item with KEY in the library.", "KEY");
    QCommandLineOption sizeOpt("size", "Window size WxH.", "WxH");
    for (auto* o : {&screenshotOpt, &openOpt, &selectOpt, &sizeOpt})
        o->setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(dataDirOpt);
    parser.addOption(configOpt);
    parser.addOption(screenshotOpt);
    parser.addOption(openOpt);
    parser.addOption(selectOpt);
    parser.addOption(sizeOpt);
    parser.addPositionalArgument("files", "PDF or other files to add to the library.", "[files...]");
    parser.process(app);

    // arguments kept when the application restarts itself (e.g. after changing the scale)
    QStringList restartArgs;
    for (auto* o : {&dataDirOpt, &configOpt})
        if (parser.isSet(*o))
            restartArgs << "--" + o->names().first() << parser.value(*o);
    app.setProperty("restartArguments", restartArgs);

    std::unique_ptr<Library> library;
    try {
        auto dataDir = parser.isSet(dataDirOpt) ? fspath(parser.value(dataDirOpt)) : config->dataDir();
        library = std::make_unique<Library>(dataDir);
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "freePen", QObject::tr("Could not open the library:\n\n%1").arg(qs(e.what())));
        return 1;
    }
    bool firstRun = library->libraries().empty();
    AppContext context(*config, *library);
    if (firstRun) {
        std::error_code ec;
        bool hasZotero = fs::exists(findZoteroDataDir() / "zotero.sqlite", ec);
        if (!hasZotero)
            library->defaultLibrary();
        MainWindow window;
        window.show();
        if (hasZotero) {
            QTimer::singleShot(300, &window, [&] {
                window.offerZoteroImport();
                if (library->libraries().empty())
                    library->defaultLibrary();
            });
        }
        return app.exec();
    }
    MainWindow window;
    if (parser.isSet(sizeOpt)) {
        auto wh = parser.value(sizeOpt).split('x');
        if (wh.size() == 2)
            window.resize(wh[0].toInt(), wh[1].toInt());
    }
    window.show();
    if (parser.isSet(selectOpt))
        QTimer::singleShot(200, &window, [&] { window.selectItem(parser.value(selectOpt)); });
    if (parser.isSet(openOpt))
        QTimer::singleShot(300, &window, [&] {
            auto key = parser.value(openOpt);
            for (auto& l : library->libraries())
                if (library->item(l.id, ss(key))) {
                    window.openAttachment(l.id, key);
                    break;
                }
        });
    if (parser.isSet(screenshotOpt)) {
        // FILE gets the main window; other open windows go to FILE-1.png, FILE-2.png, ...
        int delay = qEnvironmentVariableIsSet("FREEPEN_SCREENSHOT_DELAY")
                        ? qEnvironmentVariableIntValue("FREEPEN_SCREENSHOT_DELAY")
                        : 2500;
        QTimer::singleShot(delay, &window, [&] {
            auto file = parser.value(screenshotOpt);
            window.grab().save(file);
            int n = 0;
            for (auto* w : QApplication::topLevelWidgets())
                if (w != &window && w->isVisible() && w->isWindow())
                    w->grab().save(QString(file).replace(".png", QString("-%1.png").arg(++n)));
            app.quit();
        });
    }
    QStringList files;
    for (auto& f : parser.positionalArguments())
        if (QFileInfo(f).isFile())
            files << QFileInfo(f).absoluteFilePath();
    if (!files.isEmpty())
        QTimer::singleShot(0, &window, [&] { window.openFiles(files); });
    return app.exec();
}
