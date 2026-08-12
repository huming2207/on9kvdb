#pragma once

#include <cstdint>

inline uint32_t esp_random()
{
    static uint32_t state = UINT32_C(0x12345678);
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    return state;
}
