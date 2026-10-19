#pragma once

namespace on::gui {

// glibc keeps freed memory inside the process (per-thread arenas, and large blocks moved
// to the heap once the mmap threshold adapts), so closing reader tabs left gigabytes in
// use as far as the system could tell. These hand it back.
void configureAllocator();  // once, before threads start
void releaseFreedMemory();  // after large frees (tabs closed, caches dropped); batched
                            // and run a moment later

} // namespace on::gui
