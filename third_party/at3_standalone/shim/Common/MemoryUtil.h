// Shim for aligned allocations.
#pragma once
#include <cstdlib>
inline void *AllocateAlignedMemory(size_t size, size_t align) {
    void *p = nullptr;
    return posix_memalign(&p, align, size ? size : align) == 0 ? p : nullptr;
}
inline void FreeAlignedMemory(void *p) { std::free(p); }
