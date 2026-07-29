#include <inttypes.h>
#include <limits>
#include <unistd.h>

#include <esp_log.h>

#include "on9kvdb.hpp"

static_assert(on9kvdb_def::max_name_len == 32);
static_assert(on9kvdb_def::max_value_len == 8192);
static_assert(on9kvdb_def::max_transaction_mutations == 10);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET >= 32768);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET <=
              on9kvdb_def::runtime_memory_budget_max);
static_assert(on9kvdb_def::runtime_memory_budget_default <=
              on9kvdb_def::runtime_memory_budget_max);
static_assert(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE > 0);
static_assert(CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE > 0);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE >
              on9kvdb_def::identity_region_size);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE >
              on9kvdb_def::identity_region_size);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE %
                  on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE %
                  on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_COUNT >= 2);
static_assert(
    static_cast<uint64_t>(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE) <=
    static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT) *
        (CONFIG_ON9KVDB_SSTABLE_FILE_SIZE -
         on9kvdb_def::identity_region_size));
static_assert(
    static_cast<uint64_t>(on9kvdb_def::manifest_file_size) +
        static_cast<uint64_t>(on9kvdb_def::wal_file_count) *
            CONFIG_ON9KVDB_WAL_FILE_SIZE +
        static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT) *
            CONFIG_ON9KVDB_SSTABLE_FILE_SIZE ==
    CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE);

#if !defined(CONFIG_FATFS_SECTOR_512) && \
    !defined(CONFIG_FATFS_SECTOR_4096)
#error "on9kvdb v1 requires ESP-IDF FATFS 512-byte or 4096-byte sectors"
#endif

on9kvdb::on9kvdb(
    const char *_file_path, const on9kvdb_cfg *config) :
    file_path(_file_path)
{
    if (config != nullptr) {
        cfg = *config;
    }

    for (size_t idx = 0; idx < storage_fd_count; idx += 1) {
        storage_fds[idx] = -1;
    }
}

on9kvdb::~on9kvdb()
{
    if (deinit(false) != ESP_OK) {
        (void)deinit(true);
    }

    if (lifecycle_lock != nullptr) {
        vSemaphoreDelete(lifecycle_lock);
        lifecycle_lock = nullptr;
    }
}

on9kvdb_def::storage_geometry on9kvdb::get_build_geometry()
{
    on9kvdb_def::storage_geometry geometry = {};
    geometry.provisioned_size =
        CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE;
    geometry.max_live_bytes =
        CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE;
    geometry.manifest_size =
        on9kvdb_def::manifest_file_size;
    geometry.wal_size = CONFIG_ON9KVDB_WAL_FILE_SIZE;
    geometry.wal_count = on9kvdb_def::wal_file_count;
    geometry.table_size = CONFIG_ON9KVDB_SSTABLE_FILE_SIZE;
    geometry.table_count = CONFIG_ON9KVDB_SSTABLE_COUNT;
    geometry.alignment = on9kvdb_def::format_alignment;
    return geometry;
}

esp_err_t on9kvdb::create_locks()
{
    if (lifecycle_lock == nullptr) {
        lifecycle_lock = xSemaphoreCreateMutex();
        if (lifecycle_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t on9kvdb::validate_init_args() const
{
    if (file_path == nullptr || file_path[0] != '/' ||
        cfg.runtime_memory_budget < 32768 ||
        cfg.runtime_memory_budget >
            on9kvdb_def::runtime_memory_budget_max) {
        return ESP_ERR_INVALID_ARG;
    }

    const on9kvdb_def::storage_geometry geometry =
        get_build_geometry();
    if (!on9kvdb_def::validate_storage_geometry(geometry) ||
        geometry.manifest_size >
            static_cast<uint64_t>(
                std::numeric_limits<off_t>::max()) ||
        geometry.wal_size >
            static_cast<uint64_t>(
                std::numeric_limits<off_t>::max()) ||
        geometry.table_size >
            static_cast<uint64_t>(
                std::numeric_limits<off_t>::max())) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t on9kvdb::initialise_storage()
{
    uint64_t free_bytes = 0;
    esp_err_t ret = validate_fatfs_mount(&free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = build_manifest_path();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = setup_manifest();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = provision_all_data_files();
    if (ret != ESP_OK) {
        return ret;
    }

    return verify_canonical_file_set(false);
}

esp_err_t on9kvdb::init()
{
    esp_err_t ret = create_locks();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(lifecycle_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (initialized || shutting_down) {
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    ret = validate_init_args();
    if (ret == ESP_OK) {
        ret = initialise_storage();
    }
    if (ret == ESP_OK) {
        initialized = true;
        ESP_LOGI(
            TAG, "Init: bytes=%" PRIu64 ", live=%" PRIu64
                 ", WAL=%" PRIu32 "x%" PRIu32
                 ", tables=%" PRIu32 "x%" PRIu32,
            manifest.geometry.provisioned_size,
            manifest.geometry.max_live_bytes,
            manifest.geometry.wal_count,
            manifest.geometry.wal_size,
            manifest.geometry.table_count,
            manifest.geometry.table_size);
    } else {
        close_storage_unsafe();
    }

    xSemaphoreGive(lifecycle_lock);
    return ret;
}

void on9kvdb::close_storage_unsafe()
{
    for (size_t idx = 0; idx < storage_fd_count; idx += 1) {
        if (storage_fds[idx] >= 0) {
            (void)::close(storage_fds[idx]);
            storage_fds[idx] = -1;
        }
    }

    manifest = {};
    manifest_slot = 0;
    manifest_valid_copy_count = 0;
    manifest_path[0] = '\0';
}

esp_err_t on9kvdb::deinit(bool force)
{
    (void)force;
    if (lifecycle_lock == nullptr) {
        return ESP_OK;
    }
    if (xSemaphoreTake(lifecycle_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    shutting_down = true;
    initialized = false;
    close_storage_unsafe();
    shutting_down = false;
    xSemaphoreGive(lifecycle_lock);
    return ESP_OK;
}

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
