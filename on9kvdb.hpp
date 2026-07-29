#pragma once

#include <cstddef>
#include <cstdint>
#include <limits.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include "on9kvdb_defs.hpp"

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
    esp_err_t create_locks();
    esp_err_t validate_init_args() const;
    esp_err_t initialise_storage();
    void close_storage_unsafe();

private: // Manifest and provisioning
    esp_err_t setup_manifest();
    esp_err_t create_manifest();
    esp_err_t open_existing_manifest();
    esp_err_t load_manifest();
    esp_err_t write_manifest_copy(uint64_t generation, uint16_t state);
    esp_err_t provision_all_data_files();
    esp_err_t provision_one_data_file(
        on9kvdb_def::file_kind kind, uint32_t slot);
    esp_err_t load_file_identity(
        int file_fd, on9kvdb_def::file_kind kind, uint32_t slot,
        bool valid_copies[on9kvdb_def::identity_slot_count]) const;
    esp_err_t write_file_identity_copy(
        int file_fd, on9kvdb_def::file_kind kind, uint32_t slot,
        uint32_t copy_slot) const;

private: // FATFS and bounded I/O
    esp_err_t build_manifest_path();
    esp_err_t build_data_path(
        on9kvdb_def::file_kind kind, uint32_t slot,
        char *path_out, size_t path_out_len) const;
    esp_err_t verify_canonical_file_set(bool creating) const;
    esp_err_t validate_fatfs_mount(uint64_t *free_bytes_out) const;
    esp_err_t provision_contiguous_file(
        const char *path, uint64_t size, bool *created_out) const;
    esp_err_t validate_contiguous_file(
        const char *path, uint64_t size) const;
    esp_err_t open_file(const char *path, int *fd_out) const;
    esp_err_t read_exact_fd(
        int file_fd, uint64_t file_size, uint64_t offset,
        void *buf_out, size_t len) const;
    esp_err_t write_exact_fd(
        int file_fd, uint64_t file_size, uint64_t offset,
        const void *buf, size_t len) const;
    esp_err_t sync_fd(int file_fd) const;
    static bool parse_slot_file_name(
        const char *name, const char *prefix, uint32_t *slot_out);

private:
    static on9kvdb_def::storage_geometry get_build_geometry();
    static size_t descriptor_index(
        on9kvdb_def::file_kind kind, uint32_t slot);

private:
    static const constexpr size_t storage_fd_count =
        1U + on9kvdb_def::wal_file_count +
        CONFIG_ON9KVDB_SSTABLE_COUNT;

    const char *file_path = nullptr;
    on9kvdb_cfg cfg = {};
    char manifest_path[PATH_MAX] = {};
    int storage_fds[storage_fd_count] = {};
    on9kvdb_def::manifest_record manifest = {};
    uint32_t manifest_slot = 0;
    uint32_t manifest_valid_copy_count = 0;
    SemaphoreHandle_t lifecycle_lock = nullptr;
    bool initialized = false;
    bool shutting_down = false;

private:
    static const constexpr char TAG[] = "on9kvdb";
};
