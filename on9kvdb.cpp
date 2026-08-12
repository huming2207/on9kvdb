#include <inttypes.h>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "on9kvdb.hpp"

static_assert(on9kvdb_def::max_name_len == 128);
static_assert(on9kvdb_def::max_value_len == UINT32_MAX - 1U);
static_assert(on9kvdb_def::max_transaction_mutations == 10);
static_assert(CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS >= 1);
static_assert(CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS <= on9kvdb_def::max_transaction_mutations);
static_assert(CONFIG_ON9KVDB_MAX_NAMESPACES >= 1);
static_assert(CONFIG_ON9KVDB_MAX_NAMESPACES <= UINT16_MAX);
static_assert(CONFIG_ON9KVDB_MAX_OPEN_HANDLES >= 1);
static_assert(CONFIG_ON9KVDB_MAX_OPEN_HANDLES <= on9kvdb_def::handle_slot_capacity);
static_assert(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT >= 16);
static_assert((CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT & (CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT - 1)) == 0);
static_assert(CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE >= on9kvdb_def::inline_value_len);
static_assert(CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE >= on9kvdb_def::inline_value_len);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET >= 32768);
static_assert(CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET <= on9kvdb_def::runtime_memory_budget_max);
static_assert(on9kvdb_def::runtime_memory_budget_default <= on9kvdb_def::runtime_memory_budget_max);
static_assert(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE > 0);
static_assert(CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE > 0);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE >= on9kvdb_def::wal_record_region_offset + on9kvdb_def::wal_frame_size);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE > on9kvdb_def::identity_region_size);
static_assert(CONFIG_ON9KVDB_WAL_FILE_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_VALUE_BANK_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_VALUE_BANK_SIZE > on9kvdb_def::identity_region_size);
static_assert(CONFIG_ON9KVDB_SSTABLE_COUNT >= 4);
static_assert(CONFIG_ON9KVDB_SSTABLE_COUNT <= on9kvdb_def::max_table_count);
static_assert((CONFIG_ON9KVDB_SSTABLE_COUNT & 1U) == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE % on9kvdb_def::format_alignment == 0);
static_assert(CONFIG_ON9KVDB_SSTABLE_FILE_SIZE >
              on9kvdb_def::table_data_region_offset + CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE + on9kvdb_def::table_footer_slot_size);
static_assert((CONFIG_ON9KVDB_SSTABLE_FILE_SIZE - on9kvdb_def::table_data_region_offset - CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE -
               on9kvdb_def::table_footer_slot_size) %
                  CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE ==
              0);
static_assert(static_cast<uint64_t>((CONFIG_ON9KVDB_SSTABLE_FILE_SIZE - on9kvdb_def::table_data_region_offset -
                                     CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE - on9kvdb_def::table_footer_slot_size) /
                                    CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE) *
                  ((on9kvdb_def::table_index_entry_header_size + 2U * on9kvdb_def::max_name_len + 3U) & ~UINT64_C(3)) <=
              CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE - on9kvdb_def::table_index_header_size);
static_assert(static_cast<uint64_t>(CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE) <=
              (static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT / 2U) *
               ((CONFIG_ON9KVDB_SSTABLE_FILE_SIZE - on9kvdb_def::table_data_region_offset - CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE -
                 on9kvdb_def::table_footer_slot_size) /
                CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE) /
               2U) *
                  (CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE - on9kvdb_def::table_block_header_size));
static_assert(static_cast<uint64_t>(on9kvdb_def::manifest_file_size) +
                  static_cast<uint64_t>(on9kvdb_def::wal_file_count) * CONFIG_ON9KVDB_WAL_FILE_SIZE +
                  static_cast<uint64_t>(CONFIG_ON9KVDB_SSTABLE_COUNT) * CONFIG_ON9KVDB_SSTABLE_FILE_SIZE +
                  static_cast<uint64_t>(on9kvdb_def::value_bank_count) * CONFIG_ON9KVDB_VALUE_BANK_SIZE ==
              CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE);

namespace
{
    // One frame is the encoded-I/O scratch, one is used for unaligned read-modify-copy transfers, then readers and the
    // single writer each retain a private 4 KiB value chunk. Every byte is allocated only during init().
    static const constexpr size_t internal_io_bytes = on9kvdb_def::wal_frame_size * (3U + CONFIG_ON9KVDB_MAX_VALUE_READERS);
    static const constexpr size_t table_sort_bytes = CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT * sizeof(uint32_t);
    static const constexpr size_t table_scratch_bytes = table_sort_bytes + 2U * CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE;
    static const constexpr size_t manifest_scratch_bytes = 2U * sizeof(on9kvdb_def::manifest_record);
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

on9kvdb::on9kvdb(on9kvdb_io *new_storage, const on9kvdb_cfg *config) : storage(new_storage)
{
    if (config != nullptr) {
        cfg = *config;
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
    geometry.value_bank_size = CONFIG_ON9KVDB_VALUE_BANK_SIZE;
    geometry.value_bank_count = on9kvdb_def::value_bank_count;
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
    limits.sstable_block_bytes = CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE;
    limits.inline_value_bytes = on9kvdb_def::inline_value_len;
    return limits;
}

size_t on9kvdb::minimum_future_scratch_size()
{
    const size_t compaction_bytes = table_sort_bytes + 2U * CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE +
                                    (CONFIG_ON9KVDB_SSTABLE_COUNT / 2U) * sizeof(compaction_cursor);
    return compaction_bytes > manifest_scratch_bytes ? compaction_bytes : manifest_scratch_bytes;
}

size_t on9kvdb::minimum_runtime_memory_budget()
{
    // Each carve may consume up to alignment - 1 bytes. Include that padding so validation and allocation agree even when
    // configurable partition sizes leave the next carve unaligned.
    const size_t arena_bytes =
        (alignof(namespace_slot) - 1U) + sizeof(namespace_slot) * CONFIG_ON9KVDB_MAX_NAMESPACES + (alignof(handle_slot) - 1U) +
        sizeof(handle_slot) * CONFIG_ON9KVDB_MAX_OPEN_HANDLES + (alignof(transaction_slot) - 1U) + sizeof(transaction_slot) +
        (alignof(value_reader_slot) - 1U) + sizeof(value_reader_slot) * CONFIG_ON9KVDB_MAX_VALUE_READERS +
        (alignof(value_writer_slot) - 1U) + sizeof(value_writer_slot) + (alignof(memtable_bucket) - 1U) +
        sizeof(memtable_bucket) * CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT + (alignof(uint64_t) - 1U) +
        CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE + transaction_recovery_overhead + (alignof(uint64_t) - 1U) +
        CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE + (alignof(uint64_t) - 1U) + minimum_future_scratch_size();
    return internal_io_bytes + arena_bytes;
}

esp_err_t on9kvdb::create_locks()
{
    taskENTER_CRITICAL(&lock_creation_mux);
    const bool locks_ready = lifecycle_lock != nullptr && operation_lock != nullptr;
    taskEXIT_CRITICAL(&lock_creation_mux);
    if (locks_ready) {
        return ESP_OK;
    }

    SemaphoreHandle_t new_lifecycle_lock = xSemaphoreCreateMutex();
    if (new_lifecycle_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t new_operation_lock = xSemaphoreCreateMutex();
    if (new_operation_lock == nullptr) {
        vSemaphoreDelete(new_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    // Mutex allocation cannot run in a critical section. Install a complete pair atomically and discard a pair lost to a
    // concurrent first init().
    taskENTER_CRITICAL(&lock_creation_mux);
    if (lifecycle_lock == nullptr) {
        lifecycle_lock = new_lifecycle_lock;
        new_lifecycle_lock = nullptr;
    }
    if (operation_lock == nullptr) {
        operation_lock = new_operation_lock;
        new_operation_lock = nullptr;
    }
    const bool installed = lifecycle_lock != nullptr && operation_lock != nullptr;
    taskEXIT_CRITICAL(&lock_creation_mux);

    if (new_lifecycle_lock != nullptr) {
        vSemaphoreDelete(new_lifecycle_lock);
    }
    if (new_operation_lock != nullptr) {
        vSemaphoreDelete(new_operation_lock);
    }

    return installed ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t on9kvdb::validate_init_args() const
{
    if (storage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.runtime_memory_budget < minimum_runtime_memory_budget() ||
        cfg.runtime_memory_budget > on9kvdb_def::runtime_memory_budget_max) {
        return ESP_ERR_INVALID_SIZE;
    }

    const on9kvdb_def::storage_geometry geometry = get_build_geometry();
    const on9kvdb_def::logical_limits limits = get_build_limits();
    const uint32_t block_size = storage->block_size();
    const uint64_t block_count = storage->block_count();
    if (!on9kvdb_def::validate_compaction_capacity(geometry, limits) || block_size == 0 ||
        block_size > on9kvdb_def::format_alignment || on9kvdb_def::format_alignment % block_size != 0 ||
        geometry.provisioned_size % block_size != 0 || block_count < geometry.provisioned_size / block_size) {
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
    value_reader_buffers = io_frame + on9kvdb_def::wal_frame_size;
    io_bounce = value_reader_buffers + static_cast<size_t>(CONFIG_ON9KVDB_MAX_VALUE_READERS) * on9kvdb_def::value_chunk_size;
    value_writer_buffer = io_bounce + on9kvdb_def::wal_frame_size;

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
    allocation = nullptr;

    all_carved = all_carved && carve_arena(&cursor, &remaining, alignof(value_reader_slot),
                                           sizeof(value_reader_slot) * CONFIG_ON9KVDB_MAX_VALUE_READERS, &allocation);
    value_readers = static_cast<value_reader_slot *>(allocation);
    allocation = nullptr;
    all_carved =
        all_carved && carve_arena(&cursor, &remaining, alignof(value_writer_slot), sizeof(value_writer_slot), &allocation);
    value_writer = static_cast<value_writer_slot *>(allocation);
    allocation = nullptr;

    // The approved default budget remains 100 KiB. A caller can opt into the fixed SSTable index cache and a stable lookup
    // value buffer by supplying enough additional arena space; otherwise lookup retains its checked on-disk fallback.
    uint8_t *optional_cursor = cursor;
    size_t optional_remaining = remaining;
    void *optional_cache = nullptr;
    void *optional_value = nullptr;
    const bool optional_carved =
        carve_arena(&optional_cursor, &optional_remaining, alignof(table_index_cache_slot),
                    sizeof(table_index_cache_slot) * CONFIG_ON9KVDB_SSTABLE_COUNT, &optional_cache) &&
        carve_arena(&optional_cursor, &optional_remaining, alignof(uint64_t), on9kvdb_def::inline_value_len, &optional_value) &&
        optional_remaining >= minimum_future_scratch_size();
    if (optional_carved) {
        cursor = optional_cursor;
        remaining = optional_remaining;
        table_index_cache = static_cast<table_index_cache_slot *>(optional_cache);
        table_lookup_value = static_cast<uint8_t *>(optional_value);
        for (uint32_t slot = 0; slot < CONFIG_ON9KVDB_SSTABLE_COUNT; slot += 1U) {
            new (&table_index_cache[slot]) table_index_cache_slot{};
        }
        memset(table_lookup_value, 0, on9kvdb_def::inline_value_len);
    }

    all_carved = all_carved && carve_arena(&cursor, &remaining, alignof(uint64_t), 0, &allocation);
    if (!all_carved || remaining < minimum_future_scratch_size()) {
        reset_runtime_state_unsafe();
        return ESP_ERR_INVALID_SIZE;
    }

    future_scratch = static_cast<uint8_t *>(allocation);
    future_scratch_size = remaining;
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_NAMESPACES; idx += 1) {
        new (&namespaces[idx]) namespace_slot{};
    }
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_OPEN_HANDLES; idx += 1) {
        new (&handles[idx]) handle_slot{};
    }
    new (transaction) transaction_slot{};
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_VALUE_READERS; idx += 1U) {
        new (&value_readers[idx]) value_reader_slot{};
    }
    new (value_writer) value_writer_slot{};
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; idx += 1) {
        new (&memtable_index[idx]) memtable_bucket{};
        memtable_index[idx].record_offset = UINT32_MAX;
    }
    return ESP_OK;
}

esp_err_t on9kvdb::initialise_storage()
{
    esp_err_t ret = setup_manifest();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = provision_all_data_files();
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
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
        ESP_LOGI(TAG, "Runtime: bytes=%u, scratch=%u, SSTable index cache=%s", static_cast<unsigned>(cfg.runtime_memory_budget),
                 static_cast<unsigned>(future_scratch_size), table_index_cache == nullptr ? "disabled" : "enabled");
    } else {
        close_storage_unsafe();
    }

    xSemaphoreGive(lifecycle_lock);
    return ret;
}

esp_err_t on9kvdb::acquire_operation_lock_internal(bool allow_storage_fault) const
{
    if (lifecycle_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(lifecycle_lock, portMAX_DELAY) != pdTRUE) {
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
    if (storage_faulted && !allow_storage_fault) {
        xSemaphoreGive(operation_lock);
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(lifecycle_lock);
    return ESP_OK;
}

esp_err_t on9kvdb::acquire_operation_lock() const
{
    return acquire_operation_lock_internal(false);
}

esp_err_t on9kvdb::acquire_diagnostic_lock() const
{
    return acquire_operation_lock_internal(true);
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
    value_reader_buffers = nullptr;
    io_bounce = nullptr;
    value_writer_buffer = nullptr;
    namespaces = nullptr;
    handles = nullptr;
    transaction = nullptr;
    memtable_index = nullptr;
    transaction_staging = nullptr;
    memtable_data = nullptr;
    table_index_cache = nullptr;
    table_lookup_value = nullptr;
    future_scratch = nullptr;
    future_scratch_size = 0;
    value_readers = nullptr;
    value_writer = nullptr;
    namespace_count = 0;
    memtable_data_used = 0;
    memtable_entry_count = 0;
    next_transaction_sequence = 1;
    stats = {};
    storage_faulted = false;
    value_bank_dirty_mask = 0;
    for (uint32_t slot = 0; slot < on9kvdb_def::wal_file_count; slot += 1) {
        wal_tail[slot] = 0;
    }
}

void on9kvdb::close_storage_unsafe()
{
    manifest = {};
    manifest_slot = 0;
    manifest_valid_copy_count = 0;
    manifest_stabilization_required = false;
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
    for (uint32_t idx = 0; !resources_busy && idx < CONFIG_ON9KVDB_MAX_VALUE_READERS; idx += 1U) {
        resources_busy = value_readers[idx].used;
    }
    resources_busy = resources_busy || (value_writer != nullptr && value_writer->active);
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
