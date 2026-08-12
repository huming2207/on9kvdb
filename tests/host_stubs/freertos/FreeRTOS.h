#pragma once

#include <cstdint>

using BaseType_t = int;
using TickType_t = uint32_t;
struct host_semaphore {
};
using SemaphoreHandle_t = host_semaphore *;
using portMUX_TYPE = int;

static constexpr BaseType_t pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;

#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
