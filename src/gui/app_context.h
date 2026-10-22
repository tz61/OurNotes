#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

#include "core/config.h"
#include "core/library.h"

namespace on::gui {

// Shared application state for the GUI thread: configuration, the library
// database, and Qt signals for library changes.
class AppContext : public QObject {
    Q_OBJECT
public:
    AppContext(Config& config, Library& library, QObject* parent = nullptr);
    ~AppContext() override;

    static AppContext& get();
    static AppContext* tryGet();  // nullptr when no context exists (e.g. in unit tests)
    Config& config() { return config_; }
    Library& library() { return library_; }

    // Whether the library is writable in the GUI (read-only shared libraries are not).
    bool editable(int64_t libraryId);

    // What other programs (ournotes-cli, an agent) change while the app runs: the database
    // is looked at every 2 seconds, and then the views reload and changedElsewhere().
    void watchDatabase();
    // Around the app's own writes on other connections (a sync's thread): taken in as the
    // app's, without a signal.
    void beginOwnWrites();
    void endOwnWrites();

signals:
    // type: "item" | "collection" | "setting" | "library" | "*" (everything)
    void changed(qint64 libraryId, const QString& type, const QString& action, const QStringList& keys);
    void syncFinished();
    // Theme or formula colour settings changed.
    void appearanceChanged();
    // The PDFs' page colour (pageTone) moved: every PDF repaints, without rendering again.
    void pageToneChanged();
    // Another program wrote to the library (see watchDatabase()).
    void changedElsewhere();
    // The colours of Q, W and E (annotationPalette) changed.
    void annotationPaletteChanged();

private:
    Config& config_;
    Library& library_;
    int listener_ = 0;
    QTimer* databaseTimer_ = nullptr;
    int64_t dataVersion_ = 0;
    int ownWrites_ = 0;  // writes of the app's own going on elsewhere
};

} // namespace on::gui
