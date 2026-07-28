#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <sdkconfig.h>

#define ESP_ERR_ON9KVDB_BASE                 0xa900
#define ESP_ERR_ON9KVDB_NOT_INITIALIZED      (ESP_ERR_ON9KVDB_BASE + 0x01)
#define ESP_ERR_ON9KVDB_NOT_FOUND            (ESP_ERR_ON9KVDB_BASE + 0x02)
#define ESP_ERR_ON9KVDB_TYPE_MISMATCH        (ESP_ERR_ON9KVDB_BASE + 0x03)
#define ESP_ERR_ON9KVDB_READ_ONLY            (ESP_ERR_ON9KVDB_BASE + 0x04)
#define ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE     (ESP_ERR_ON9KVDB_BASE + 0x05)
#define ESP_ERR_ON9KVDB_INVALID_NAME         (ESP_ERR_ON9KVDB_BASE + 0x06)
#define ESP_ERR_ON9KVDB_INVALID_HANDLE       (ESP_ERR_ON9KVDB_BASE + 0x07)
#define ESP_ERR_ON9KVDB_INVALID_LENGTH       (ESP_ERR_ON9KVDB_BASE + 0x08)
#define ESP_ERR_ON9KVDB_VALUE_TOO_LONG       (ESP_ERR_ON9KVDB_BASE + 0x09)
#define ESP_ERR_ON9KVDB_TRANSACTION_TOO_LARGE \
    (ESP_ERR_ON9KVDB_BASE + 0x0a)
#define ESP_ERR_ON9KVDB_CORRUPT              (ESP_ERR_ON9KVDB_BASE + 0x0b)
#define ESP_ERR_ON9KVDB_NEW_VERSION_FOUND    (ESP_ERR_ON9KVDB_BASE + 0x0c)
#define ESP_ERR_ON9KVDB_INCOMPATIBLE_GEOMETRY \
    (ESP_ERR_ON9KVDB_BASE + 0x0d)
#define ESP_ERR_ON9KVDB_BUSY                 (ESP_ERR_ON9KVDB_BASE + 0x0e)

const char *on9kvdb_err_to_name(esp_err_t error);

enum class on9kvdb_open_mode : uint8_t {
    read_only = 0,
    read_write = 1,
};

enum class on9kvdb_type : uint8_t {
    i8 = 0x01,
    u8 = 0x02,
    i16 = 0x03,
    u16 = 0x04,
    i32 = 0x05,
    u32 = 0x06,
    i64 = 0x07,
    u64 = 0x08,
    str = 0x09,
    blob = 0x0a,
    any = 0xff,
};

class on9kvdb_handle
{
public:
    constexpr on9kvdb_handle() = default;

    [[nodiscard]] constexpr bool is_valid() const
    {
        return raw != 0;
    }

private:
    explicit constexpr on9kvdb_handle(uint32_t value) : raw(value)
    {
    }

private:
    uint32_t raw = 0;

    friend class on9kvdb;
};

class on9kvdb_transaction_handle
{
public:
    constexpr on9kvdb_transaction_handle() = default;

    [[nodiscard]] constexpr bool is_valid() const
    {
        return raw != 0;
    }

private:
    explicit constexpr on9kvdb_transaction_handle(uint32_t value) : raw(value)
    {
    }

private:
    uint32_t raw = 0;

    friend class on9kvdb;
};

struct on9kvdb_cfg {
    size_t runtime_memory_budget =
        CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET;
};

struct on9kvdb_stats {
    uint64_t namespace_count = 0;
    uint64_t live_key_count = 0;
    uint64_t tombstone_count = 0;
    uint64_t committed_transaction_count = 0;
    uint64_t logical_value_bytes = 0;
    uint64_t wal_bytes_used = 0;
    uint64_t table_bytes_used = 0;
};

class on9kvdb
{
public:
    explicit on9kvdb(const char *file_path, const on9kvdb_cfg *config);
    ~on9kvdb();

public:
    esp_err_t init();
    esp_err_t deinit(bool force = false);

    esp_err_t open(const char *namespace_name, on9kvdb_open_mode mode,
                   on9kvdb_handle *handle_out);
    esp_err_t close(on9kvdb_handle handle);

    esp_err_t begin(on9kvdb_handle handle,
                    on9kvdb_transaction_handle *transaction_out);
    esp_err_t commit(on9kvdb_transaction_handle transaction);
    esp_err_t close(on9kvdb_transaction_handle transaction);
    esp_err_t abort(on9kvdb_transaction_handle transaction);

    esp_err_t set_i8(on9kvdb_transaction_handle transaction,
                     const char *key, int8_t value);
    esp_err_t set_u8(on9kvdb_transaction_handle transaction,
                     const char *key, uint8_t value);
    esp_err_t set_i16(on9kvdb_transaction_handle transaction,
                      const char *key, int16_t value);
    esp_err_t set_u16(on9kvdb_transaction_handle transaction,
                      const char *key, uint16_t value);
    esp_err_t set_i32(on9kvdb_transaction_handle transaction,
                      const char *key, int32_t value);
    esp_err_t set_u32(on9kvdb_transaction_handle transaction,
                      const char *key, uint32_t value);
    esp_err_t set_i64(on9kvdb_transaction_handle transaction,
                      const char *key, int64_t value);
    esp_err_t set_u64(on9kvdb_transaction_handle transaction,
                      const char *key, uint64_t value);
    esp_err_t set_str(on9kvdb_transaction_handle transaction,
                      const char *key, const char *value);
    esp_err_t set_blob(on9kvdb_transaction_handle transaction,
                       const char *key, const void *value, size_t length);
    esp_err_t erase_key(on9kvdb_transaction_handle transaction,
                        const char *key);

    esp_err_t get_i8(on9kvdb_handle handle, const char *key,
                     int8_t *value_out) const;
    esp_err_t get_u8(on9kvdb_handle handle, const char *key,
                     uint8_t *value_out) const;
    esp_err_t get_i16(on9kvdb_handle handle, const char *key,
                      int16_t *value_out) const;
    esp_err_t get_u16(on9kvdb_handle handle, const char *key,
                      uint16_t *value_out) const;
    esp_err_t get_i32(on9kvdb_handle handle, const char *key,
                      int32_t *value_out) const;
    esp_err_t get_u32(on9kvdb_handle handle, const char *key,
                      uint32_t *value_out) const;
    esp_err_t get_i64(on9kvdb_handle handle, const char *key,
                      int64_t *value_out) const;
    esp_err_t get_u64(on9kvdb_handle handle, const char *key,
                      uint64_t *value_out) const;
    esp_err_t get_str(on9kvdb_handle handle, const char *key,
                      char *value_out, size_t *length) const;
    esp_err_t get_blob(on9kvdb_handle handle, const char *key,
                       void *value_out, size_t *length) const;

    esp_err_t get_i8(on9kvdb_transaction_handle transaction,
                     const char *key, int8_t *value_out) const;
    esp_err_t get_u8(on9kvdb_transaction_handle transaction,
                     const char *key, uint8_t *value_out) const;
    esp_err_t get_i16(on9kvdb_transaction_handle transaction,
                      const char *key, int16_t *value_out) const;
    esp_err_t get_u16(on9kvdb_transaction_handle transaction,
                      const char *key, uint16_t *value_out) const;
    esp_err_t get_i32(on9kvdb_transaction_handle transaction,
                      const char *key, int32_t *value_out) const;
    esp_err_t get_u32(on9kvdb_transaction_handle transaction,
                      const char *key, uint32_t *value_out) const;
    esp_err_t get_i64(on9kvdb_transaction_handle transaction,
                      const char *key, int64_t *value_out) const;
    esp_err_t get_u64(on9kvdb_transaction_handle transaction,
                      const char *key, uint64_t *value_out) const;
    esp_err_t get_str(on9kvdb_transaction_handle transaction,
                      const char *key, char *value_out,
                      size_t *length) const;
    esp_err_t get_blob(on9kvdb_transaction_handle transaction,
                       const char *key, void *value_out,
                       size_t *length) const;

    esp_err_t find_key(on9kvdb_handle handle, const char *key,
                       on9kvdb_type *type_out) const;
    esp_err_t find_key(on9kvdb_transaction_handle transaction,
                       const char *key, on9kvdb_type *type_out) const;
    esp_err_t get_stats(on9kvdb_stats *stats_out) const;

private:
    const char *file_path = nullptr;
    on9kvdb_cfg cfg = {};
};
