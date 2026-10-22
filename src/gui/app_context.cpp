#include "gui/app_context.h"

#include "gui/common.h"

namespace on::gui {

static AppContext* g_context = nullptr;

AppContext::AppContext(Config& config, Library& library, QObject* parent)
    : QObject(parent), config_(config), library_(library)
{
    g_context = this;
    listener_ = library_.subscribe([this](const ChangeEvent& ev) {
        emit changed(ev.libraryId, qs(ev.type), qs(ev.action), toQStringList(ev.keys));
    });
}

AppContext::~AppContext()
{
    library_.unsubscribe(listener_);
    g_context = nullptr;
}

AppContext& AppContext::get() { return *g_context; }
AppContext* AppContext::tryGet() { return g_context; }

void AppContext::watchDatabase()
{
    if (databaseTimer_)
        return;
    dataVersion_ = library_.dataVersion();
    databaseTimer_ = new QTimer(this);
    databaseTimer_->setInterval(2000);
    connect(databaseTimer_, &QTimer::timeout, this, [this] {
        auto v = library_.dataVersion();
        if (v == dataVersion_ || ownWrites_ > 0)
            return;
        dataVersion_ = v;
        library_.notifyRefresh();
        emit changedElsewhere();
    });
    databaseTimer_->start();
}

void AppContext::beginOwnWrites() { ++ownWrites_; }

void AppContext::endOwnWrites()
{
    if (ownWrites_ > 0 && --ownWrites_ == 0)
        dataVersion_ = library_.dataVersion();  // (what was written meanwhile is taken as the app's)
}

bool AppContext::editable(int64_t libraryId)
{
    auto info = library_.library(libraryId);
    return info && info->editable;
}

} // namespace on::gui
