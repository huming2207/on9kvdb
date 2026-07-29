#include <inttypes.h>
#include <limits>
#include <cstring>
#include <new>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "on9kvdb.hpp"

static_assert(on9kvdb_def::max_name_len == 32);
static_assert(on9kvdb_def::max_value_len == 8192);
static_assert(on9kvdb_def::max_transaction_mutations == 10);
static_assert(CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS >= 1);
static_assert(CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS <= on9kvdb_def::max_transaction_mutations);
static_assert(CONFIG_ON9KVDB_MAX_NAMESPACES >= 1);
static_assert(CONFIG_ON9KVDB_MAX_NAMESPACES <= UINT16_MAX);
static_assert(CONFIG_ON9KVDB_MAX_OPEN_HANDLES >= 1);
static_assert(CONFIG_ON9KVDB_MAX_OPEN_HANDLES <= UINT16_MAX);
static_assert(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT >= 16);
static_assert((CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT & (CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT - 1)) == 0);
static_assert(CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE >= on9kvdb_def::max_value_len);
static_assert(CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE >= on9kvdb_def::max_value_len);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET >= 32768);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET <= on9kvdb_def::runtime_memory_budget_max);
static_assert(on9kvdb_def::runtime_memory_budget_default <= on9kvdb_def::runtime_memory_budget_max);
static_assert(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE > 0);
static_assert(CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE > 0);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE > on9kvdb_def::wal_record_region_offset);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE > on9kvdb_def::identity_region_size);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_COUNT >= 2);
static_assert(static_cast<uint64_t>(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE) <=
              static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT) *
                  (CONFIG_ON9KVDB_SSTABLE_FILE_SIZE - on9kvdb_def::identity_region_size));
static_assert(static_cast<uint64_t>(on9kvdb_def::manifest_file_size) +
                  static_cast<uint64_t>(on9kvdb_def::wal_file_count) * CONFIG_ON9KVDB_WAL_FILE_SIZE +
                  static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT) * CONFIG_ON9KVDB_SSTABLE_FILE_SIZE ==
              CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE);

#if !defined(CONFIG_FATFS_SECTOR_512) && !defined(CONFIG_FATFS_SECTOR_4096)
#error "on9kvdb v1 requires ESP-IDF FATFS 512-byte or 4096-byte sectors"
#endif

namespace
{
    static const constexpr size_t internal_io_bytes = on9kvdb_def::wal_frame_size;
    static const constexpr size_t minimum_future_scratch_bytes = 16U * 1024U;
    static const constexpr size_t transaction_recovery_overhead =
        8U + on9kvdb_def::max_name_len + on9kvdb_def::max_transaction_mutations * (8U + on9kvdb_def::max_name_len);

    bool carve_arena(uint8_t **cursor, size_t *remaining, size_t alignment, size_t size, void **result_out)
    {
        if (cursor == nullptr || *cursor == nullptr || remaining == nullptr || result_out == nullptr || alignment == 0 ||
            (alignment & (alignment - 1U)) != 0) {
            return false;
        }

        const uintptr_t address = reinterpret_cast<uintptr_t>(*cursor);
        const uintptr_t aligned = (address + alignment - 1U) & ~(static_cast<uintptr_t>(alignment) - 1U);
        const size_t padding = static_cast<size_t>(aligned - address);
        if (padding > *remaining || size > *remaining - padding) {
            return false;
        }

        *result_out = reinterpret_cast<void *>(aligned);
        *cursor += padding + size;
        *remaining -= padding + size;
        return true;
    }
}

on9kvdb::on9kvdb(const char *_file_path, const on9kvdb_cfg *config) : file_path(_file_path)
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

    if (operation_lock != nullptr) {
        vSemaphoreDelete(operation_lock);
        operation_lock = nullptr;
    }
}

on9kvdb_def::storage_geometry on9kvdb::get_build_geometry()
{
    on9kvdb_def::storage_geometry geometry = {};
    geometry.provisioned_size = CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE;
    geometry.max_live_bytes = CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE;
    geometry.manifest_size = on9kvdb_def::manifest_file_size;
    geometry.wal_size = CONFIG_ON9KVDB_WAL_FILE_SIZE;
    geometry.wal_count = on9kvdb_def::wal_file_count;
    geometry.table_size = CONFIG_ON9KVDB_SSTABLE_FILE_SIZE;
    geometry.table_count = CONFIG_ON9KVDB_SSTABLE_COUNT;
    geometry.alignment = on9kvdb_def::format_alignment;
    return geometry;
}

on9kvdb_def::logical_limits on9kvdb::get_build_limits()
{
    on9kvdb_def::logical_limits limits = {};
    limits.wal_frame_bytes = on9kvdb_def::wal_frame_size;
    limits.max_namespaces = CONFIG_ON9KVDB_MAX_NAMESPACES;
    limits.max_open_handles = CONFIG_ON9KVDB_MAX_OPEN_HANDLES;
    limits.memtable_entries = CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT;
    limits.memtable_data_bytes = CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE;
    limits.max_transaction_mutations = CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS;
    limits.transaction_staging_bytes = CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE;
    return limits;
}

esp_err_t on9kvdb::create_locks()
{
    if (lifecycle_lock == nullptr) {
        lifecycle_lock = xSemaphoreCreateMutex();
        if (lifecycle_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (operation_lock == nullptr) {
        operation_lock = xSemaphoreCreateMutex();
        if (operation_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t on9kvdb::validate_init_args() const
{
    if (file_path == nullptr || file_path[0] != '/' ||
        cfg.runtime_memory_budget < internal_io_bytes + minimum_future_scratch_bytes ||
        cfg.runtime_memory_budget > on9kvdb_def::runtime_memory_budget_max) {
        return ESP_ERR_INVALID_ARG;
    }

    const on9kvdb_def::storage_geometry geometry = get_build_geometry();
    const on9kvdb_def::logical_limits limits = get_build_limits();
    if (!on9kvdb_def::validate_storage_geometry(geometry) || !on9kvdb_def::validate_logical_limits(limits) ||
        geometry.manifest_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        geometry.wal_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        geometry.table_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t on9kvdb::allocate_runtime_memory()
{
    if (runtime_arena != nullptr || io_frame != nullptr || cfg.runtime_memory_budget <= internal_io_bytes) {
        return ESP_ERR_INVALID_STATE;
    }

    io_frame = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(64, internal_io_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (io_frame == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    memset(io_frame, 0, internal_io_bytes);

    runtime_arena_size = cfg.runtime_memory_budget - internal_io_bytes;
    uint32_t arena_caps = MALLOC_CAP_8BIT;
#if CONFIG_ON9KVDB_REQUIRE_PSRAM
#if CONFIG_SPIRAM
    arena_caps |= MALLOC_CAP_SPIRAM;
#else
    heap_caps_free(io_frame);
    io_frame = nullptr;
    runtime_arena_size = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
#else
    arena_caps |= MALLOC_CAP_INTERNAL;
#endif

    runtime_arena = heap_caps_aligned_alloc(16, runtime_arena_size, arena_caps);
    if (runtime_arena == nullptr) {
        heap_caps_free(io_frame);
        io_frame = nullptr;
        runtime_arena_size = 0;
        return ESP_ERR_NO_MEM;
    }
    memset(runtime_arena, 0, runtime_arena_size);

    uint8_t *cursor = static_cast<uint8_t *>(runtime_arena);
    size_t remaining = runtime_arena_size;
    void *allocation = nullptr;
    const bool carved = carve_arena(&cursor, &remaining, alignof(namespace_slot),
                                    sizeof(namespace_slot) * CONFIG_ON9KVDB_MAX_NAMESPACES, &allocation);
    namespaces = static_cast<namespace_slot *>(allocation);
    allocation = nullptr;

    bool all_carved = carved && carve_arena(&cursor, &remaining, alignof(handle_slot),
                                            sizeof(handle_slot) * CONFIG_ON9KVDB_MAX_OPEN_HANDLES, &allocation);
    handles = static_cast<handle_slot *>(allocation);
    allocation = nullptr;

    all_carved = all_carved && carve_arena(&cursor, &remaining, alignof(transaction_slot), sizeof(transaction_slot), &allocation);
    transaction = static_cast<transaction_slot *>(allocation);
    allocation = nullptr;

    all_carved = all_carved && carve_arena(&cursor, &remaining, alignof(memtable_bucket),
                                           sizeof(memtable_bucket) * CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT, &allocation);
    memtable_index = static_cast<memtable_bucket *>(allocation);
    allocation = nullptr;

    all_carved = all_carved && carve_arena(&cursor, &remaining, alignof(uint64_t),
                                           CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE + transaction_recovery_overhead, &allocation);
    transaction_staging = static_cast<uint8_t *>(allocation);
    allocation = nullptr;

    all_carved =
        all_carved && carve_arena(&cursor, &remaining, alignof(uint64_t), CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE, &allocation);
    memtable_data = static_cast<uint8_t *>(allocation);
    if (!all_carved || remaining < minimum_future_scratch_bytes) {
        reset_runtime_state_unsafe();
        return ESP_ERR_INVALID_SIZE;
    }

    future_scratch = cursor;
    future_scratch_size = remaining;
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_NAMESPACES; idx += 1) {
        new (&namespaces[idx]) namespace_slot{};
    }
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_OPEN_HANDLES; idx += 1) {
        new (&handles[idx]) handle_slot{};
    }
    new (transaction) transaction_slot{};
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; idx += 1) {
        new (&memtable_index[idx]) memtable_bucket{};
        memtable_index[idx].record_offset = UINT32_MAX;
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

esp_err_t on9kvdb::finish_initialisation()
{
    return recover_wal();
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
        ret = allocate_runtime_memory();
    }
    if (ret == ESP_OK) {
        ret = initialise_storage();
    }
    if (ret == ESP_OK) {
        ret = finish_initialisation();
    }
    if (ret == ESP_OK) {
        initialized = true;
        ESP_LOGI(TAG, "Init: bytes=%" PRIu64 ", live=%" PRIu64 ", WAL=%" PRIu32 "x%" PRIu32 ", tables=%" PRIu32 "x%" PRIu32,
                 manifest.geometry.provisioned_size, manifest.geometry.max_live_bytes, manifest.geometry.wal_count,
                 manifest.geometry.wal_size, manifest.geometry.table_count, manifest.geometry.table_size);
    } else {
        close_storage_unsafe();
    }

    xSemaphoreGive(lifecycle_lock);
    return ret;
}

esp_err_t on9kvdb::acquire_operation_lock() const
{
    if (lifecycle_lock == nullptr || xSemaphoreTake(lifecycle_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!initialized || shutting_down || operation_lock == nullptr) {
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(operation_lock, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(lifecycle_lock);
    return ESP_OK;
}

void on9kvdb::release_operation_lock() const
{
    xSemaphoreGive(operation_lock);
}

void on9kvdb::reset_runtime_state_unsafe()
{
    if (runtime_arena != nullptr) {
        heap_caps_free(runtime_arena);
    }
    if (io_frame != nullptr) {
        heap_caps_free(io_frame);
    }

    runtime_arena = nullptr;
    runtime_arena_size = 0;
    io_frame = nullptr;
    namespaces = nullptr;
    handles = nullptr;
    transaction = nullptr;
    memtable_index = nullptr;
    transaction_staging = nullptr;
    memtable_data = nullptr;
    future_scratch = nullptr;
    future_scratch_size = 0;
    namespace_count = 0;
    memtable_data_used = 0;
    memtable_entry_count = 0;
    next_transaction_sequence = 1;
    stats = {};
    for (uint32_t slot = 0; slot < on9kvdb_def::wal_file_count; slot += 1) {
        wal_tail[slot] = 0;
    }
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
    reset_runtime_state_unsafe();
}

esp_err_t on9kvdb::deinit(bool force)
{
    if (lifecycle_lock == nullptr) {
        return ESP_OK;
    }
    if (xSemaphoreTake(lifecycle_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!initialized) {
        xSemaphoreGive(lifecycle_lock);
        return ESP_OK;
    }

    shutting_down = true;
    if (xSemaphoreTake(operation_lock, portMAX_DELAY) != pdTRUE) {
        shutting_down = false;
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_TIMEOUT;
    }

    bool resources_busy = transaction != nullptr && transaction->active;
    for (uint32_t idx = 0; !resources_busy && idx < CONFIG_ON9KVDB_MAX_OPEN_HANDLES; idx += 1) {
        resources_busy = handles[idx].used;
    }
    if (resources_busy && !force) {
        xSemaphoreGive(operation_lock);
        shutting_down = false;
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    initialized = false;
    close_storage_unsafe();
    xSemaphoreGive(operation_lock);
    shutting_down = false;
    xSemaphoreGive(lifecycle_lock);
    return ESP_OK;
}
