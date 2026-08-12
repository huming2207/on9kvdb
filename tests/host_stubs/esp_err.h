#pragma once

#include <cstdint>

using esp_err_t = int32_t;

static constexpr esp_err_t ESP_OK = 0;
static constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
static constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
static constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
static constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
static constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
static constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;
static constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
static constexpr esp_err_t ESP_ERR_INVALID_RESPONSE = 0x108;
static constexpr esp_err_t ESP_ERR_INVALID_CRC = 0x109;
static constexpr esp_err_t ESP_ERR_INVALID_VERSION = 0x10a;
static constexpr esp_err_t ESP_ERR_NOT_ALLOWED = 0x10b;

inline const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "ESP_ERROR";
}
