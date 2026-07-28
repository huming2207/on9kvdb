#include "on9kvdb.hpp"
#include "on9kvdb_defs.hpp"

static_assert(on9kvdb_def::max_name_len == 32);
static_assert(on9kvdb_def::max_value_len == 8192);
static_assert(on9kvdb_def::max_transaction_mutations == 10);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET >= 32768);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET <=
              on9kvdb_def::runtime_memory_budget_max);
static_assert(on9kvdb_def::runtime_memory_budget_default <=
              on9kvdb_def::runtime_memory_budget_max);

const char *on9kvdb_err_to_name(esp_err_t error)
{
    switch (error) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_ERR_ON9KVDB_NOT_INITIALIZED:
        return "ESP_ERR_ON9KVDB_NOT_INITIALIZED";
    case ESP_ERR_ON9KVDB_NOT_FOUND:
        return "ESP_ERR_ON9KVDB_NOT_FOUND";
    case ESP_ERR_ON9KVDB_TYPE_MISMATCH:
        return "ESP_ERR_ON9KVDB_TYPE_MISMATCH";
    case ESP_ERR_ON9KVDB_READ_ONLY:
        return "ESP_ERR_ON9KVDB_READ_ONLY";
    case ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE:
        return "ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE";
    case ESP_ERR_ON9KVDB_INVALID_NAME:
        return "ESP_ERR_ON9KVDB_INVALID_NAME";
    case ESP_ERR_ON9KVDB_INVALID_HANDLE:
        return "ESP_ERR_ON9KVDB_INVALID_HANDLE";
    case ESP_ERR_ON9KVDB_INVALID_LENGTH:
        return "ESP_ERR_ON9KVDB_INVALID_LENGTH";
    case ESP_ERR_ON9KVDB_VALUE_TOO_LONG:
        return "ESP_ERR_ON9KVDB_VALUE_TOO_LONG";
    case ESP_ERR_ON9KVDB_TRANSACTION_TOO_LARGE:
        return "ESP_ERR_ON9KVDB_TRANSACTION_TOO_LARGE";
    case ESP_ERR_ON9KVDB_CORRUPT:
        return "ESP_ERR_ON9KVDB_CORRUPT";
    case ESP_ERR_ON9KVDB_NEW_VERSION_FOUND:
        return "ESP_ERR_ON9KVDB_NEW_VERSION_FOUND";
    case ESP_ERR_ON9KVDB_INCOMPATIBLE_GEOMETRY:
        return "ESP_ERR_ON9KVDB_INCOMPATIBLE_GEOMETRY";
    case ESP_ERR_ON9KVDB_BUSY:
        return "ESP_ERR_ON9KVDB_BUSY";
    default:
        return "ESP_ERR_ON9KVDB_UNKNOWN";
    }
}
