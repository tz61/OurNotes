#include "gui/memory.h"

#include <QTimer>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace on::gui {

void configureAllocator()
{
#ifdef __GLIBC__
    // a reader tab has a render thread; one arena per thread fragments memory
    mallopt(M_ARENA_MAX, 4);
#endif
}

void releaseFreedMemory()
{
#ifdef __GLIBC__
    static QTimer* timer = [] {
        auto* t = new QTimer;
        t->setSingleShot(true);
        t->setInterval(1000);
        QObject::connect(t, &QTimer::timeout, [] { malloc_trim(0); });
        return t;
    }();
    timer->start();
#endif
}

} // namespace on::gui
