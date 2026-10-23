#include "gui/ui_scale.h"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace on::gui {

static bool g_fromEnvironment = false;
static bool g_setHere = false;

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
    if (percent != 100) {
        qputenv("QT_SCALE_FACTOR", QByteArray::number(percent / 100.0));
        g_setHere = true;
    }
}

bool interfaceScaleFromEnvironment() { return g_fromEnvironment; }

void forgetInterfaceScale()
{
    if (std::exchange(g_setHere, false))
        qunsetenv("QT_SCALE_FACTOR");  // (Qt reads it once, as the QApplication is created)
}

} // namespace on::gui
