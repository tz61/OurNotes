#include "gui/input_methods.h"

#include <QByteArray>
#include <QtGlobal>

namespace on::gui {

void chooseInputMethods()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
    if (!qEnvironmentVariableIsEmpty("QT_IM_MODULES"))
        return;  // the user's list
    QByteArray im = qgetenv("QT_IM_MODULE");
    if (im.toLower() == "fcitx" || im.toLower() == "fcitx5")
        qputenv("QT_IM_MODULES", im + ";ibus");
#endif
}

} // namespace on::gui
