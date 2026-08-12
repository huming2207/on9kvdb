#pragma once

#include <cstdlib>

static constexpr unsigned MALLOC_CAP_INTERNAL = 1U;
static constexpr unsigned MALLOC_CAP_DMA = 2U;
static constexpr unsigned MALLOC_CAP_8BIT = 4U;
static constexpr unsigned MALLOC_CAP_SPIRAM = 8U;

inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned)
{
    void *memory = nullptr;
    return posix_memalign(&memory, alignment, size) == 0 ? memory : nullptr;
}

inline void heap_caps_free(void *memory)
{
    free(memory);
}
