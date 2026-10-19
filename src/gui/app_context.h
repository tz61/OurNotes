#pragma once

#include <QObject>
#include <QStringList>

#include "core/config.h"
#include "core/library.h"

namespace fp::gui {

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

signals:
    // type: "item" | "collection" | "setting" | "library" | "*" (everything)
    void changed(qint64 libraryId, const QString& type, const QString& action, const QStringList& keys);
    void syncFinished();
    // Theme or formula colour settings changed.
    void appearanceChanged();

private:
    Config& config_;
    Library& library_;
    int listener_ = 0;
};

} // namespace fp::gui
