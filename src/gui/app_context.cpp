#include "gui/app_context.h"

#include "gui/common.h"

namespace fp::gui {

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

bool AppContext::editable(int64_t libraryId)
{
    auto info = library_.library(libraryId);
    return info && info->editable;
}

} // namespace fp::gui
