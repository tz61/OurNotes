#include "gui/memory.h"

#include <QTimer>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace on::gui {

void configureAllocator()
{
#ifdef __GLIBC__
    // large blocks (rendered pages, decoded images) get their own mappings, returned to
    // the system when freed; setting the threshold also stops it from growing
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
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
