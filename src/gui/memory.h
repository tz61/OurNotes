#pragma once

namespace on::gui {

// glibc keeps freed memory inside the process (spread over per-thread arenas), so closing
// reader tabs left gigabytes in use as far as the system could tell. These hand it back.
// (A fixed mmap threshold would return page images at once too, but costs ~6 ms per
// rendered page for fresh, zeroed memory: rendering was twice as slow.)
void configureAllocator();  // once, before threads start
void releaseFreedMemory();  // after large frees (tabs closed, caches dropped); batched
                            // and run a moment later

} // namespace on::gui
