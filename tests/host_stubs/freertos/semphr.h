#pragma once

#include <cstdlib>

#include "FreeRTOS.h"

inline SemaphoreHandle_t xSemaphoreCreateMutex()
{
    return static_cast<SemaphoreHandle_t>(malloc(sizeof(host_semaphore)));
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t)
{
    return semaphore == nullptr ? 0 : pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore == nullptr ? 0 : pdTRUE;
}

inline void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    free(semaphore);
}
