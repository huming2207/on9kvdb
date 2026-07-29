#pragma once

#include <cstddef>
#include <cstdint>
#include <limits.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include "on9kvdb_defs.hpp"

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
    explicit constexpr on9kvdb_handle(uint32_t value) : raw(value) {}

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
    explicit constexpr on9kvdb_transaction_handle(uint32_t value) : raw(value) {}

private:
    uint32_t raw = 0;

    friend class on9kvdb;
};

struct on9kvdb_cfg {
    size_t runtime_memory_budget = CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET;
};

struct on9kvdb_stats {
    uint64_t namespace_count = 0;
    uint64_t live_key_count = 0;
    uint64_t tombstone_count = 0;
    uint64_t committed_transaction_count = 0;
    uint64_t logical_value_bytes = 0;
    uint64_t logical_state_bytes = 0;
    uint64_t wal_bytes_used = 0;
    uint64_t table_bytes_used = 0;
    uint64_t compaction_count = 0;
    uint64_t compaction_input_record_bytes = 0;
    uint64_t compaction_output_record_bytes = 0;
    uint64_t last_compaction_time_us = 0;
    uint64_t maximum_compaction_time_us = 0;
    uint64_t logical_state_capacity_bytes = 0;
    uint64_t wal_record_capacity_bytes = 0;
    uint64_t provisioned_database_bytes = 0;
    uint64_t runtime_memory_bytes = 0;
    uint64_t runtime_scratch_bytes = 0;
    uint64_t manifest_generation = 0;
    uint64_t safe_checkpoint_sequence = 0;
    uint32_t active_table_count = 0;
    uint32_t referenced_wal_count = 0;
    uint32_t valid_manifest_copy_count = 0;
    uint32_t active_table_bank = 0;
    uint32_t active_wal_slot = 0;
    bool manifest_stabilization_required = false;
    bool storage_faulted = false;
};

class on9kvdb
{
public:
    explicit on9kvdb(const char *file_path, const on9kvdb_cfg *config);
    ~on9kvdb();

public:
    esp_err_t init();
    esp_err_t deinit(bool force = false);

    esp_err_t open(const char *namespace_name, on9kvdb_open_mode mode, on9kvdb_handle *handle_out);
    esp_err_t close(on9kvdb_handle handle);

    esp_err_t begin(on9kvdb_handle handle, on9kvdb_transaction_handle *transaction_out);
    esp_err_t commit(on9kvdb_transaction_handle transaction);
    esp_err_t close(on9kvdb_transaction_handle transaction);
    esp_err_t abort(on9kvdb_transaction_handle transaction);

    esp_err_t set_i8(on9kvdb_transaction_handle transaction, const char *key, int8_t value);
    esp_err_t set_u8(on9kvdb_transaction_handle transaction, const char *key, uint8_t value);
    esp_err_t set_i16(on9kvdb_transaction_handle transaction, const char *key, int16_t value);
    esp_err_t set_u16(on9kvdb_transaction_handle transaction, const char *key, uint16_t value);
    esp_err_t set_i32(on9kvdb_transaction_handle transaction, const char *key, int32_t value);
    esp_err_t set_u32(on9kvdb_transaction_handle transaction, const char *key, uint32_t value);
    esp_err_t set_i64(on9kvdb_transaction_handle transaction, const char *key, int64_t value);
    esp_err_t set_u64(on9kvdb_transaction_handle transaction, const char *key, uint64_t value);
    esp_err_t set_str(on9kvdb_transaction_handle transaction, const char *key, const char *value);
    esp_err_t set_blob(on9kvdb_transaction_handle transaction, const char *key, const void *value, size_t length);
    esp_err_t erase_key(on9kvdb_transaction_handle transaction, const char *key);

    esp_err_t get_i8(on9kvdb_handle handle, const char *key, int8_t *value_out) const;
    esp_err_t get_u8(on9kvdb_handle handle, const char *key, uint8_t *value_out) const;
    esp_err_t get_i16(on9kvdb_handle handle, const char *key, int16_t *value_out) const;
    esp_err_t get_u16(on9kvdb_handle handle, const char *key, uint16_t *value_out) const;
    esp_err_t get_i32(on9kvdb_handle handle, const char *key, int32_t *value_out) const;
    esp_err_t get_u32(on9kvdb_handle handle, const char *key, uint32_t *value_out) const;
    esp_err_t get_i64(on9kvdb_handle handle, const char *key, int64_t *value_out) const;
    esp_err_t get_u64(on9kvdb_handle handle, const char *key, uint64_t *value_out) const;
    esp_err_t get_str(on9kvdb_handle handle, const char *key, char *value_out, size_t *length) const;
    esp_err_t get_blob(on9kvdb_handle handle, const char *key, void *value_out, size_t *length) const;

    esp_err_t get_i8(on9kvdb_transaction_handle transaction, const char *key, int8_t *value_out) const;
    esp_err_t get_u8(on9kvdb_transaction_handle transaction, const char *key, uint8_t *value_out) const;
    esp_err_t get_i16(on9kvdb_transaction_handle transaction, const char *key, int16_t *value_out) const;
    esp_err_t get_u16(on9kvdb_transaction_handle transaction, const char *key, uint16_t *value_out) const;
    esp_err_t get_i32(on9kvdb_transaction_handle transaction, const char *key, int32_t *value_out) const;
    esp_err_t get_u32(on9kvdb_transaction_handle transaction, const char *key, uint32_t *value_out) const;
    esp_err_t get_i64(on9kvdb_transaction_handle transaction, const char *key, int64_t *value_out) const;
    esp_err_t get_u64(on9kvdb_transaction_handle transaction, const char *key, uint64_t *value_out) const;
    esp_err_t get_str(on9kvdb_transaction_handle transaction, const char *key, char *value_out, size_t *length) const;
    esp_err_t get_blob(on9kvdb_transaction_handle transaction, const char *key, void *value_out, size_t *length) const;

    esp_err_t find_key(on9kvdb_handle handle, const char *key, on9kvdb_type *type_out) const;
    esp_err_t find_key(on9kvdb_transaction_handle transaction, const char *key, on9kvdb_type *type_out) const;
    esp_err_t get_stats(on9kvdb_stats *stats_out) const;

private:
    struct namespace_slot {
        bool used;
        uint8_t name_size;
        char name[on9kvdb_def::max_name_len + 1];
    };

    struct handle_slot {
        uint16_t generation;
        bool used;
        bool transaction_active;
        on9kvdb_open_mode mode;
        uint8_t namespace_size;
        char namespace_name[on9kvdb_def::max_name_len + 1];
    };

    struct mutation_slot {
        uint32_t value_offset;
        uint32_t value_size;
        uint32_t previous_value_size;
        uint8_t key_size;
        uint8_t type;
        uint8_t kind;
        uint8_t previous_state;
        char key[on9kvdb_def::max_name_len + 1];
    };

    struct transaction_slot {
        uint16_t generation;
        uint16_t handle_slot_index;
        uint16_t handle_generation;
        uint16_t mutation_count;
        uint32_t staged_value_bytes;
        bool active;
        mutation_slot mutations[on9kvdb_def::max_transaction_mutations];
    };

    struct memtable_bucket {
        uint32_t hash;
        uint32_t record_offset;
        uint32_t record_size;
    };

    struct memtable_record_header {
        uint64_t transaction_sequence;
        uint32_t total_size;
        uint32_t value_size;
        uint16_t namespace_slot_index;
        uint8_t key_size;
        uint8_t type;
        uint8_t flags;
        uint8_t reserved[3];
    };

    static_assert(sizeof(memtable_record_header) == 24);
    static_assert(alignof(memtable_record_header) <= alignof(uint64_t));

    struct value_view {
        const uint8_t *value = nullptr;
        uint64_t transaction_sequence = 0;
        uint32_t value_size = 0;
        on9kvdb_type type = on9kvdb_type::any;
        bool tombstone = false;
    };

    struct wal_payload_copy_state {
        uint32_t stream_offset;
        size_t request_size;
        uint8_t *destination;
        uint32_t logical_offset;
        size_t copied;
    };

    struct table_build_state {
        int file_fd = -1;
        uint8_t *data_block = nullptr;
        uint8_t *index_block = nullptr;
        uint64_t generation = 0;
        uint32_t slot = 0;
        uint32_t data_block_count = 0;
        uint32_t entry_count = 0;
        uint32_t data_bytes = 0;
        uint32_t data_payload_size = 0;
        uint32_t index_payload_size = 0;
        uint32_t content_crc = UINT32_MAX;
        uint16_t data_block_entry_count = 0;
    };

    struct compaction_cursor {
        on9kvdb_def::composite_key key = {};
        uint64_t transaction_sequence = 0;
        uint32_t source_slot = 0;
        uint32_t block_index = 0;
        uint32_t entry_offset = 0;
        uint32_t block_payload_end = 0;
        uint32_t total_size = 0;
        uint32_t value_size = 0;
        uint16_t entry_index = 0;
        uint16_t block_entry_count = 0;
        uint8_t type = 0;
        uint8_t flags = 0;
        bool active = false;
    };

    struct compaction_output {
        table_build_state build = {};
        on9kvdb_def::table_metadata metadata = {};
        uint32_t maximum_data_blocks = 0;
        bool active = false;
    };

private:
    static size_t minimum_future_scratch_size();
    esp_err_t create_locks();
    esp_err_t validate_init_args() const;
    esp_err_t allocate_runtime_memory();
    esp_err_t initialise_storage();
    esp_err_t finish_initialisation();
    void reset_runtime_state_unsafe();
    void close_storage_unsafe();
    esp_err_t acquire_operation_lock_internal(bool allow_storage_fault) const;
    esp_err_t acquire_operation_lock() const;
    esp_err_t acquire_diagnostic_lock() const;
    void release_operation_lock() const;

private: // Handles and transactions
    esp_err_t get_handle_slot_unsafe(on9kvdb_handle handle, handle_slot **slot_out, uint16_t *slot_index_out = nullptr);
    esp_err_t get_handle_slot_unsafe(on9kvdb_handle handle, const handle_slot **slot_out,
                                     uint16_t *slot_index_out = nullptr) const;
    esp_err_t get_transaction_unsafe(on9kvdb_transaction_handle transaction, transaction_slot **slot_out);
    esp_err_t get_transaction_unsafe(on9kvdb_transaction_handle transaction, const transaction_slot **slot_out) const;
    void clear_transaction_unsafe();
    esp_err_t stage_value_unsafe(on9kvdb_transaction_handle transaction, const char *key, on9kvdb_type type, const void *value,
                                 size_t value_size, uint8_t mutation_kind);
    mutation_slot *find_staged_mutation_unsafe(transaction_slot *transaction, const char *key);
    const mutation_slot *find_staged_mutation_unsafe(const transaction_slot *transaction, const char *key) const;

private: // Manifest and provisioning
    esp_err_t setup_manifest();
    esp_err_t create_manifest();
    esp_err_t open_existing_manifest();
    esp_err_t load_manifest();
    esp_err_t write_manifest_copy(uint64_t generation, uint16_t state);
    esp_err_t stabilize_manifest_unsafe();
    esp_err_t provision_all_data_files();
    esp_err_t provision_one_data_file(on9kvdb_def::file_kind kind, uint32_t slot);
    esp_err_t load_file_identity(int file_fd, on9kvdb_def::file_kind kind, uint32_t slot,
                                 bool valid_copies[on9kvdb_def::identity_slot_count]) const;
    esp_err_t write_file_identity_copy(int file_fd, on9kvdb_def::file_kind kind, uint32_t slot, uint32_t copy_slot) const;

private: // WAL
    esp_err_t initialise_first_wal();
    esp_err_t ensure_wal_header(uint32_t slot, uint64_t generation, uint64_t first_transaction_sequence);
    esp_err_t load_wal_header(uint32_t slot, uint64_t expected_generation, on9kvdb_def::wal_header *header_out) const;
    esp_err_t recover_wal();
    esp_err_t scan_wal_slot(uint32_t slot, uint64_t generation, uint64_t *expected_sequence);
    esp_err_t rotate_wal_unsafe();
    esp_err_t append_transaction_unsafe(transaction_slot *transaction, const handle_slot &handle);
    esp_err_t calculate_transaction_payload_unsafe(const transaction_slot &transaction, const handle_slot &handle,
                                                   uint32_t *payload_size_out, uint32_t *checksum_out) const;
    esp_err_t copy_transaction_payload_unsafe(const transaction_slot &transaction, const handle_slot &handle,
                                              uint32_t stream_offset, uint8_t *destination, size_t destination_size) const;
    static void copy_wal_payload_segment_unsafe(wal_payload_copy_state *copy_state, const uint8_t *data, size_t size);
    esp_err_t parse_recovered_transaction_unsafe(const uint8_t *payload, size_t payload_size, uint16_t expected_mutation_count,
                                                 char namespace_name[on9kvdb_def::max_name_len + 1]);

private: // Immutable SSTables
    esp_err_t flush_memtable_unsafe();
    esp_err_t compact_tables_unsafe();
    esp_err_t load_compaction_cursor_unsafe(compaction_cursor *cursor);
    esp_err_t advance_compaction_cursor_unsafe(compaction_cursor *cursor);
    esp_err_t start_compaction_output_unsafe(compaction_output *output, uint32_t slot, uint64_t generation, uint8_t *data_block,
                                             uint8_t *index_block);
    esp_err_t append_compaction_entry_unsafe(compaction_output *output, const compaction_cursor *table_cursor,
                                             const memtable_record_header *memtable_record,
                                             const namespace_slot *memtable_namespace);
    esp_err_t finish_compaction_output_unsafe(compaction_output *output, on9kvdb_def::table_reference *reference_out);
    esp_err_t recover_tables_unsafe();
    esp_err_t validate_table_unsafe(const on9kvdb_def::table_reference &reference);
    esp_err_t lookup_tables_unsafe(const char *namespace_name, const char *key, value_view *view_out) const;
    esp_err_t lookup_table_unsafe(const on9kvdb_def::table_reference &reference, const on9kvdb_def::composite_key &key,
                                  value_view *view_out) const;
    esp_err_t lookup_committed_unsafe(const char *namespace_name, const char *key, value_view *view_out) const;
    esp_err_t read_table_bytes_unsafe(uint32_t slot, uint64_t offset, uint8_t *destination, size_t size) const;
    esp_err_t write_table_bytes_unsafe(uint32_t slot, uint64_t offset, const uint8_t *source, size_t size);
    esp_err_t finish_table_data_block_unsafe(table_build_state *state);
    int compare_memtable_records_unsafe(uint32_t lhs_offset, uint32_t rhs_offset) const;
    void sift_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count, uint32_t root) const;
    void sort_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count) const;
    void reset_memtable_unsafe();

private: // Memtable and namespace registry
    esp_err_t find_namespace_unsafe(const char *namespace_name, uint16_t *slot_index_out) const;
    esp_err_t ensure_namespace_capacity_unsafe(const char *namespace_name, uint16_t *slot_index_out, bool publish);
    uint32_t hash_key_unsafe(uint16_t namespace_slot_index, const char *key, size_t key_size) const;
    esp_err_t find_memtable_bucket_unsafe(uint16_t namespace_slot_index, const char *key, size_t key_size,
                                          uint32_t *bucket_index_out, bool *found_out) const;
    esp_err_t lookup_memtable_unsafe(const char *namespace_name, const char *key, value_view *view_out) const;
    esp_err_t lookup_transaction_unsafe(const transaction_slot &transaction, const handle_slot &handle, const char *key,
                                        value_view *view_out) const;
    esp_err_t preflight_memtable_transaction_unsafe(transaction_slot &transaction, const char *namespace_name,
                                                    uint16_t *namespace_slot_out);
    void compact_memtable_unsafe();
    void remove_memtable_record_unsafe(uint32_t bucket_index);
    esp_err_t apply_transaction_to_memtable_unsafe(const transaction_slot &transaction, uint16_t namespace_slot_index,
                                                   uint64_t transaction_sequence);
    esp_err_t get_fixed_value_unsafe(const value_view &view, on9kvdb_type expected_type, void *value_out,
                                     size_t expected_size) const;
    esp_err_t get_variable_value_unsafe(const value_view &view, on9kvdb_type expected_type, void *value_out,
                                        size_t *length) const;

private: // FATFS and bounded I/O
    esp_err_t build_manifest_path();
    esp_err_t build_data_path(on9kvdb_def::file_kind kind, uint32_t slot, char *path_out, size_t path_out_len) const;
    esp_err_t verify_canonical_file_set(bool creating) const;
    esp_err_t validate_fatfs_mount(uint64_t *free_bytes_out) const;
    esp_err_t provision_contiguous_file(const char *path, uint64_t size, bool *created_out) const;
    esp_err_t validate_contiguous_file(const char *path, uint64_t size) const;
    esp_err_t open_file(const char *path, int *fd_out) const;
    esp_err_t read_exact_fd(int file_fd, uint64_t file_size, uint64_t offset, void *buf_out, size_t len) const;
    esp_err_t write_exact_fd(int file_fd, uint64_t file_size, uint64_t offset, const void *buf, size_t len) const;
    esp_err_t sync_fd(int file_fd) const;
    static bool parse_slot_file_name(const char *name, const char *prefix, uint32_t *slot_out);

private:
    static on9kvdb_def::storage_geometry get_build_geometry();
    static on9kvdb_def::logical_limits get_build_limits();
    static size_t descriptor_index(on9kvdb_def::file_kind kind, uint32_t slot);

private:
    static const constexpr size_t storage_fd_count = 1U + on9kvdb_def::wal_file_count + CONFIG_ON9KVDB_SSTABLE_COUNT;

    const char *file_path = nullptr;
    on9kvdb_cfg cfg = {};
    char manifest_path[PATH_MAX] = {};
    int storage_fds[storage_fd_count] = {};
    on9kvdb_def::manifest_record manifest = {};
    uint32_t manifest_slot = 0;
    uint32_t manifest_valid_copy_count = 0;
    bool manifest_stabilization_required = false;
    SemaphoreHandle_t lifecycle_lock = nullptr;
    SemaphoreHandle_t operation_lock = nullptr;

    void *runtime_arena = nullptr;
    size_t runtime_arena_size = 0;
    uint8_t *io_frame = nullptr;
    namespace_slot *namespaces = nullptr;
    handle_slot *handles = nullptr;
    transaction_slot *transaction = nullptr;
    memtable_bucket *memtable_index = nullptr;
    uint8_t *transaction_staging = nullptr;
    uint8_t *memtable_data = nullptr;
    uint8_t *future_scratch = nullptr;
    size_t future_scratch_size = 0;
    uint32_t namespace_count = 0;
    uint32_t memtable_data_used = 0;
    uint32_t memtable_entry_count = 0;
    uint32_t wal_tail[on9kvdb_def::wal_file_count] = {};
    uint64_t next_transaction_sequence = 1;
    on9kvdb_stats stats = {};
    bool initialized = false;
    bool shutting_down = false;
    bool storage_faulted = false;

private:
    static const constexpr char TAG[] = "on9kvdb";
};
