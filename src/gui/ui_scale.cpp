#include "gui/ui_scale.h"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>

namespace fp::gui {

static bool g_fromEnvironment = false;

int interfaceScale(const Config& config)
{
    auto v = config.get("/ui/scale", kDefaultInterfaceScale);
    int percent = v.is_number() ? (int)v.get<double>() : kDefaultInterfaceScale;
    return std::clamp(percent, 50, 300);
}

void applyInterfaceScale(const Config& config)
{
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        g_fromEnvironment = true;
        return;
    }
    int percent = interfaceScale(config);
    if (percent != 100)
        qputenv("QT_SCALE_FACTOR", QByteArray::number(percent / 100.0));
}

bool interfaceScaleFromEnvironment() { return g_fromEnvironment; }

} // namespace fp::gui
