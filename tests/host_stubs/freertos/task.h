#pragma once

#include "FreeRTOS.h"

inline uint64_t host_task_delay_call_count = 0;

inline void vTaskDelay(TickType_t)
{
    host_task_delay_call_count += 1U;
}
