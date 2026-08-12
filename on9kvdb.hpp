#pragma once

#include <cstddef>
#include <cstdint>
#include <limits.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include "on9kvdb_defs.hpp"
#include "on9kvdb_io.hpp"

/**
 * @brief Access permissions requested when opening a namespace.
 */
enum class on9kvdb_open_mode : uint8_t {
    /** Open an existing namespace for committed-value reads only. */
    read_only = 0,
    /** Open a namespace for reads and for one active write transaction. */
    read_write = 1,
};

/**
 * @brief An opaque binary namespace or key.
 *
 * Names are byte sequences rather than C strings: embedded `0x00` bytes are
 * valid data and comparisons use the complete explicit length. The caller
 * retains ownership of @p data; the database copies the name before the call
 * returns.
 */
struct on9kvdb_bytes {
    /** Pointer to the first name byte; it must be non-null when @c size is non-zero. */
    const uint8_t *data = nullptr;
    /** Name length in bytes. Valid namespaces and keys are 1 through 128 bytes. */
    uint16_t size = 0;
};

/**
 * @brief Opaque handle to one opened namespace.
 *
 * Obtain a handle with on9kvdb::open() and release it with
 * on9kvdb::close(on9kvdb_handle). It is invalidated when closed or when the
 * owning database is deinitialized.
 */
class on9kvdb_handle
{
public:
    constexpr on9kvdb_handle() = default;

    /**
     * @brief Check whether this wrapper contains a non-zero handle token.
     *
     * @note This is a cheap local check only. It does not prove that the
     * handle is still open or belongs to a particular database instance.
     */
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

/**
 * @brief Opaque handle to the database's single active write transaction.
 *
 * Create it with on9kvdb::begin(), then commit it with on9kvdb::commit() or
 * discard it with on9kvdb::abort(). A transaction belongs to one read-write
 * namespace handle.
 */
class on9kvdb_transaction_handle
{
public:
    constexpr on9kvdb_transaction_handle() = default;

    /** @brief Check whether this wrapper contains a non-zero transaction token. */
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

/**
 * @brief Opaque streaming reader for one committed value.
 *
 * Readers are fixed-capacity resources allocated by init(). An open external
 * value pins its storage bank until on9kvdb::close(on9kvdb_value_reader) is
 * called, preventing compaction from recycling that bank.
 */
class on9kvdb_value_reader
{
public:
    constexpr on9kvdb_value_reader() = default;

    /** @brief Check whether this wrapper contains a non-zero reader token. */
    [[nodiscard]] constexpr bool is_valid() const
    {
        return raw != 0;
    }

private:
    explicit constexpr on9kvdb_value_reader(uint32_t value) : raw(value) {}

private:
    uint32_t raw = 0;

    friend class on9kvdb;
};

/**
 * @brief Opaque sequential writer for one value in the active transaction.
 *
 * Only one value writer may be active per database. Finish it with
 * on9kvdb::finish_value_write() or discard it with
 * on9kvdb::abort_value_write().
 */
class on9kvdb_value_writer
{
public:
    constexpr on9kvdb_value_writer() = default;

    /** @brief Check whether this wrapper contains a non-zero writer token. */
    [[nodiscard]] constexpr bool is_valid() const
    {
        return raw != 0;
    }

private:
    explicit constexpr on9kvdb_value_writer(uint32_t value) : raw(value) {}

private:
    uint32_t raw = 0;

    friend class on9kvdb;
};

/**
 * @brief Runtime-memory configuration supplied when constructing a database.
 */
struct on9kvdb_cfg {
    /**
     * Total RAM budget, in bytes, reserved by init() for indexes, mutation
     * staging, readers, the writer, and I/O scratch memory. The value must
     * satisfy the component's configured minimum and maximum limits.
     */
    size_t runtime_memory_budget = CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET;
};

/**
 * @brief Snapshot of runtime capacity, usage, compaction, and recovery state.
 *
 * Obtain this structure with on9kvdb::get_stats(). All byte fields are counts
 * in bytes and all timing fields are microseconds.
 */
struct on9kvdb_stats {
    uint64_t namespace_count = 0;                 /**< Number of known namespaces. */
    uint64_t live_key_count = 0;                  /**< Number of committed non-tombstone keys. */
    uint64_t tombstone_count = 0;                 /**< Number of committed tombstone records. */
    uint64_t committed_transaction_count = 0;     /**< Transactions recovered or committed in this database. */
    uint64_t logical_value_bytes = 0;             /**< Sum of live value lengths, excluding record overhead. */
    uint64_t logical_state_bytes = 0;             /**< Live logical state, including namespace/key metadata. */
    uint64_t wal_bytes_used = 0;                  /**< Bytes occupied by retained WAL records. */
    uint64_t table_bytes_used = 0;                /**< Bytes occupied by active SSTable records. */
    uint64_t compaction_count = 0;                /**< Completed table/value-bank compactions since init. */
    uint64_t compaction_input_record_bytes = 0;   /**< Cumulative record bytes read by compaction. */
    uint64_t compaction_output_record_bytes = 0;  /**< Cumulative record bytes emitted by compaction. */
    uint64_t last_compaction_time_us = 0;         /**< Duration of the most recent compaction. */
    uint64_t maximum_compaction_time_us = 0;      /**< Longest compaction duration observed. */
    uint64_t logical_state_capacity_bytes = 0;    /**< Configured maximum logical-state capacity. */
    uint64_t wal_record_capacity_bytes = 0;       /**< Total record payload capacity of referenced WAL slots. */
    uint64_t provisioned_database_bytes = 0;      /**< Total fixed database storage provisioned in the raw block range. */
    uint64_t runtime_memory_bytes = 0;            /**< Runtime arena budget selected at construction. */
    uint64_t runtime_scratch_bytes = 0;           /**< Runtime arena bytes reserved for future scratch work. */
    uint64_t manifest_generation = 0;             /**< Generation of the selected manifest copy. */
    uint64_t safe_checkpoint_sequence = 0;        /**< Highest transaction checkpointed into SSTables. */
    uint32_t active_table_count = 0;              /**< Number of SSTables referenced by the manifest. */
    uint32_t referenced_wal_count = 0;            /**< Number of WAL slots referenced by the manifest. */
    uint32_t valid_manifest_copy_count = 0;       /**< Number of independently valid manifest copies. */
    uint32_t active_table_bank = 0;               /**< Currently published SSTable-bank index. */
    uint32_t active_wal_slot = 0;                 /**< WAL slot receiving the next committed transaction. */
    bool manifest_stabilization_required = false; /**< True when recovery must rewrite a missing manifest mirror. */
    bool storage_faulted = false;                 /**< True after an unsafe post-WAL publication failure. */
};

class on9kvdb
{
public:
    /**
     * @brief Construct an uninitialized database instance.
     *
     * @param storage Exclusive raw block device. It must be initialized before
     * init(), remain valid until deinit() completes, and expose at least the
     * configured provisioned database capacity.
     * @param config Optional runtime-memory configuration. Passing @c nullptr
     * selects the Kconfig default.
     *
     * @note Construction performs no allocation or storage I/O. Call init()
     * before any other database operation.
     */
    explicit on9kvdb(on9kvdb_io *storage, const on9kvdb_cfg *config);

    /**
     * @brief Release database resources.
     *
     * The destructor first attempts a normal deinit(), then forces teardown if
     * application code left a handle, transaction, reader, or writer open.
     */
    ~on9kvdb();

public:
    /**
     * @brief Allocate fixed runtime memory, open/provision storage, and recover committed state.
     *
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_STATE if the instance is already initialized.
     * @return Other ESP-IDF error codes for invalid configuration, insufficient
     * memory, unavailable storage, or detected storage corruption.
     *
     * @note This is the only normal operation that allocates component-owned
     * RAM. All reader and writer buffers are reserved here.
     */
    esp_err_t init();

    /**
     * @brief Close storage and release the memory reserved by init().
     *
     * @param force When false, fail if an open namespace handle, transaction,
     * reader, or writer remains. When true, discard those in-memory resources
     * and tear down the instance.
     * @return ESP_OK on success or if the database is already deinitialized.
     * @return ESP_ERR_INVALID_STATE if resources remain open and @p force is false.
     * @return ESP_ERR_TIMEOUT if an internal lock cannot be acquired.
     */
    esp_err_t deinit(bool force = false);

    /**
     * @brief Open a binary namespace and return a namespace handle.
     *
     * @param namespace_name Namespace bytes, from 1 through 128 bytes. The
     * slice can contain embedded zero bytes and is copied before this call returns.
     * @param mode Requested access mode.
     * @param[out] handle_out Receives an open namespace handle on success.
     * @return ESP_OK on success.
     * @return ESP_ERR_NOT_FOUND if a read-only open names a namespace that has
     * not been committed.
     * @return ESP_ERR_NO_MEM if no configured handle slot is available.
     * @return ESP_ERR_INVALID_ARG for an invalid byte slice, mode, or output pointer.
     *
     * @note A read-write open may reserve a new namespace. It becomes durable
     * only when a transaction commits a value in it.
     */
    esp_err_t open(on9kvdb_bytes namespace_name, on9kvdb_open_mode mode, on9kvdb_handle *handle_out);

    /**
     * @brief Stage one complete opaque value in the active transaction.
     *
     * @param transaction Active transaction returned by begin().
     * @param key Binary key bytes, from 1 through 128 bytes.
     * @param value Value bytes, or @c nullptr only when @p value_size is zero.
     * The caller retains ownership of the input buffer.
     * @param value_size Number of value bytes, from zero through
     * @c UINT32_MAX - 1, subject to configured database capacity.
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_SIZE if transaction staging or available value
     * storage cannot accommodate the value.
     * @return ESP_ERR_INVALID_STATE for an invalid or inactive transaction.
     *
     * @note This is the convenience path for a contiguous buffer. Values that
     * exceed the inline limit use the same fixed-buffer value-bank path as the
     * progressive writer API; no whole-value heap allocation is made.
     */
    esp_err_t set(on9kvdb_transaction_handle transaction, on9kvdb_bytes key, const uint8_t *value, uint32_t value_size);

    /**
     * @brief Stage deletion of a key in the active transaction.
     *
     * @param transaction Active transaction returned by begin().
     * @param key Binary key bytes, from 1 through 128 bytes.
     * @return ESP_OK on success.
     * @return ESP_ERR_NOT_FOUND if the key is not visible in the transaction's
     * current view.
     * @return ESP_ERR_INVALID_ARG or ESP_ERR_INVALID_STATE for invalid input
     * or a non-active transaction.
     */
    esp_err_t erase_key(on9kvdb_transaction_handle transaction, on9kvdb_bytes key);

    /**
     * @brief Open a streaming reader for the committed value of a key.
     *
     * @param handle Open namespace handle.
     * @param key Binary key bytes, from 1 through 128 bytes.
     * @param[out] reader_out Receives an open reader on success.
     * @return ESP_OK on success.
     * @return ESP_ERR_NOT_FOUND if no committed live value exists for @p key.
     * @return ESP_ERR_NO_MEM if every configured reader slot is in use.
     * @return ESP_ERR_INVALID_CRC if an external-value descriptor is corrupt.
     *
     * @note The reader observes committed state only, never the staged overlay
     * of an active transaction. It pins an external value bank until closed.
     */
    esp_err_t open_value(on9kvdb_handle handle, on9kvdb_bytes key, on9kvdb_value_reader *reader_out);

    /**
     * @brief Get the total byte length of an open value reader.
     *
     * @param reader Open value reader.
     * @param[out] size_out Receives the value size in bytes.
     * @return ESP_OK on success or an error for an invalid reader or null output pointer.
     *
     * @note This operation does not move the reader cursor.
     */
    esp_err_t get_value_size(on9kvdb_value_reader reader, uint32_t *size_out) const;

    /**
     * @brief Expose the next contiguous bytes at the reader cursor without copying.
     *
     * @param reader Open value reader.
     * @param[out] data_out Receives a pointer into the reader's fixed private buffer.
     * @param[out] size_out Receives the number of consecutive bytes available
     * from the current cursor. It is zero at end of value.
     * @return ESP_OK on success, including end of value.
     * @return ESP_ERR_INVALID_ARG for null output pointers.
     * @return ESP_ERR_INVALID_CRC for a corrupt external-value chunk.
     *
     * @note The span remains valid only until the next operation on this
     * reader or close(on9kvdb_value_reader). Call consume_value() with no more
     * than @p size_out bytes to advance the cursor.
     */
    esp_err_t peek_value(on9kvdb_value_reader reader, const uint8_t **data_out, uint32_t *size_out);

    /**
     * @brief Advance a value reader by a caller-selected number of bytes.
     *
     * @param reader Open value reader.
     * @param size Number of bytes to consume from its current cursor.
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_SIZE if @p size exceeds the remaining value bytes.
     *
     * @note @p size may cross a peek_value() span boundary. A subsequent
     * peek_value() refills the reader buffer when necessary.
     */
    esp_err_t consume_value(on9kvdb_value_reader reader, uint32_t size);

    /**
     * @brief Copy available bytes into caller-owned memory and advance the cursor.
     *
     * @param reader Open value reader.
     * @param destination Destination buffer, or @c nullptr when
     * @p destination_size is zero.
     * @param destination_size Capacity of @p destination in bytes.
     * @param[out] read_size_out Receives the number of bytes copied, from zero
     * through the smaller of @p destination_size and the bytes remaining.
     * @return ESP_OK on success, including a short read at end of value.
     * @return ESP_ERR_INVALID_ARG for invalid pointers.
     * @return ESP_ERR_INVALID_CRC if an external value chunk is corrupt.
     *
     * @note This is the normal scalar/small-value path. It never allocates and
     * does not require the caller to manage a peek_value() span.
     */
    esp_err_t read_value_into(on9kvdb_value_reader reader, void *destination, uint32_t destination_size, uint32_t *read_size_out);

    /**
     * @brief Move a value reader cursor to an absolute byte offset.
     *
     * @param reader Open value reader.
     * @param offset New offset in the range zero through the value size,
     * inclusive. An offset equal to the value size seeks to end of value.
     * @return ESP_OK on success or ESP_ERR_INVALID_SIZE for an out-of-range offset.
     */
    esp_err_t seek_value(on9kvdb_value_reader reader, uint32_t offset);

    /**
     * @brief Close a value reader and release its fixed reader slot and bank pin.
     *
     * @param reader Open value reader.
     * @return ESP_OK on success or ESP_ERR_INVALID_STATE for a stale reader.
     */
    esp_err_t close(on9kvdb_value_reader reader);

    /**
     * @brief Start a sequential, exact-length value write in an active transaction.
     *
     * @param transaction Active transaction returned by begin().
     * @param key Binary key bytes, from 1 through 128 bytes.
     * @param value_size Exact final value size in bytes, from zero through
     * @c UINT32_MAX - 1, subject to configured storage capacity.
     * @param[out] writer_out Receives an active writer on success.
     * @return ESP_OK on success.
     * @return ESP_ERR_NO_MEM if a value-bank compaction cannot create enough
     * contiguous chunk capacity.
     * @return ESP_ERR_INVALID_STATE if another writer is active or the
     * transaction is invalid.
     *
     * @note The fixed final length lets the database select inline storage or
     * reserve an exact chunk count without a heap buffer or extent map.
     */
    esp_err_t begin_value_write(on9kvdb_transaction_handle transaction, on9kvdb_bytes key, uint32_t value_size,
                                on9kvdb_value_writer *writer_out);

    /**
     * @brief Append the next bytes of a progressive value write.
     *
     * @param writer Active value writer.
     * @param data Input bytes, or @c nullptr only when @p size is zero. The
     * bytes are copied before the call returns.
     * @param size Number of bytes to append.
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_ARG if @p size would exceed the declared final
     * value size or if the input pointer is invalid.
     *
     * @note Writes are strictly sequential. This operation may flush completed
     * fixed-size chunks, but the value is not visible until the transaction commits.
     */
    esp_err_t write_value(on9kvdb_value_writer writer, const uint8_t *data, uint32_t size);

    /**
     * @brief Finish a progressive writer and stage its value in the transaction.
     *
     * @param writer Active value writer.
     * @return ESP_OK when exactly the declared byte count has been supplied.
     * @return ESP_ERR_INVALID_STATE if bytes are missing or the writer is invalid.
     *
     * @note This invalidates the writer token. Call commit() separately to
     * make the staged value durable and visible.
     */
    esp_err_t finish_value_write(on9kvdb_value_writer writer);

    /**
     * @brief Abandon a progressive writer without staging its value.
     *
     * @param writer Active value writer.
     * @return ESP_OK on success or ESP_ERR_INVALID_STATE for a stale writer.
     *
     * @note Already-written external chunks become unreachable and are
     * reclaimed during a later value-bank-switching compaction.
     */
    esp_err_t abort_value_write(on9kvdb_value_writer writer);

    /**
     * @brief Close a namespace handle.
     *
     * @param handle Open namespace handle.
     * @return ESP_OK on success.
     * @return ESP_ERR_INVALID_STATE if @p handle is stale or still owns an
     * active transaction.
     */
    esp_err_t close(on9kvdb_handle handle);

    /**
     * @brief Begin the database's single active write transaction.
     *
     * @param handle Open read-write namespace handle.
     * @param[out] transaction_out Receives an active transaction on success.
     * @return ESP_OK on success.
     * @return ESP_ERR_NOT_ALLOWED if @p handle is read-only.
     * @return ESP_ERR_INVALID_STATE if this handle, or the database, already
     * has an active transaction.
     */
    esp_err_t begin(on9kvdb_handle handle, on9kvdb_transaction_handle *transaction_out);

    /**
     * @brief Durably commit all mutations staged in a transaction.
     *
     * @param transaction Active transaction to commit.
     * @return ESP_OK once the transaction is durable and visible to committed readers.
     * @return An ESP-IDF storage or capacity error if the commit cannot safely
     * complete. A post-WAL publication failure faults the instance to prevent
     * further unsafe operations.
     */
    esp_err_t commit(on9kvdb_transaction_handle transaction);

    /**
     * @brief Commit a transaction; synonym for commit().
     *
     * @param transaction Active transaction to commit.
     * @return The result of commit().
     */
    esp_err_t close(on9kvdb_transaction_handle transaction);

    /**
     * @brief Discard all mutations staged in a transaction.
     *
     * @param transaction Active transaction to abort.
     * @return ESP_OK on success or ESP_ERR_INVALID_STATE for a stale transaction.
     *
     * @note External chunks written by an unfinished or aborted transaction are
     * unreachable and are reclaimed by later compaction.
     */
    esp_err_t abort(on9kvdb_transaction_handle transaction);

    /**
     * @brief Get a consistent diagnostics snapshot.
     *
     * @param[out] stats_out Receives the current statistics and configuration-derived capacity values.
     * @return ESP_OK on success or ESP_ERR_INVALID_ARG when @p stats_out is null.
     *
     * @note This API remains available after a storage fault so callers can
     * diagnose the instance state.
     */
    esp_err_t get_stats(on9kvdb_stats *stats_out) const;

private:
    struct namespace_slot {
        bool used;
        uint16_t name_size;
        uint8_t name[on9kvdb_def::max_name_len];
    };

    struct handle_slot {
        uint32_t generation;
        bool used;
        bool transaction_active;
        on9kvdb_open_mode mode;
        uint16_t namespace_size;
        uint8_t namespace_name[on9kvdb_def::max_name_len];
    };

    struct mutation_slot {
        uint32_t value_offset;
        uint32_t value_size;
        uint32_t previous_value_size;
        uint16_t key_size;
        uint8_t reserved0;
        uint8_t kind;
        uint8_t previous_state;
        bool external_value;
        on9kvdb_def::value_ref external_value_ref;
        uint8_t key[on9kvdb_def::max_name_len];
    };

    struct transaction_slot {
        uint32_t generation;
        uint16_t handle_slot_index;
        uint32_t handle_generation;
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
        uint16_t key_size;
        uint8_t reserved0;
        uint8_t flags;
        uint8_t reserved[2];
        on9kvdb_def::value_ref external_value;
    };

    static_assert(sizeof(memtable_record_header) == 48);
    static_assert(alignof(memtable_record_header) <= alignof(uint64_t));

    struct value_view {
        const uint8_t *value = nullptr;
        uint64_t transaction_sequence = 0;
        uint32_t value_size = 0;
        on9kvdb_def::value_ref external_value = {};
        bool tombstone = false;
        bool is_external = false;
    };

    struct value_reader_slot {
        uint32_t generation = 0;
        uint32_t value_size = 0;
        uint32_t cursor = 0;
        uint32_t buffer_size = 0;
        uint32_t buffer_value_offset = 0;
        on9kvdb_def::value_ref external_value = {};
        bool used = false;
        bool external = false;
    };

    struct value_writer_slot {
        uint64_t checksum_state = UINT32_MAX;
        uint32_t transaction_handle_raw = 0;
        uint32_t generation = 0;
        uint32_t expected_size = 0;
        uint32_t written_size = 0;
        uint32_t chunk_payload_size = 0;
        uint32_t next_chunk_offset = 0;
        uint16_t key_size = 0;
        uint8_t key[on9kvdb_def::max_name_len] = {};
        on9kvdb_def::value_ref reference = {};
        bool active = false;
        bool inline_value = false;
    };

    static const constexpr uint32_t maximum_table_data_blocks =
        (CONFIG_ON9KVDB_SSTABLE_FILE_SIZE - on9kvdb_def::table_data_region_offset - CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE -
         on9kvdb_def::table_footer_slot_size) /
        CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE;

    struct table_index_cache_entry {
        on9kvdb_def::composite_key first_key = {};
        uint32_t block_offset = 0;
    };

    struct table_index_cache_slot {
        uint64_t generation = 0;
        uint32_t entry_count = 0;
        bool valid = false;
        table_index_cache_entry entries[maximum_table_data_blocks] = {};
    };

    struct wal_payload_copy_state {
        uint32_t stream_offset;
        size_t request_size;
        uint8_t *destination;
        uint32_t logical_offset;
        size_t copied;
    };

    struct table_build_state {
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
        on9kvdb_def::value_ref external_value = {};
        uint16_t entry_index = 0;
        uint16_t block_entry_count = 0;
        uint8_t reserved0 = 0;
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
    static size_t minimum_runtime_memory_budget();
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

private: // Value banks and streaming handles
    esp_err_t get_value_reader_unsafe(on9kvdb_value_reader reader, value_reader_slot **slot_out);
    esp_err_t get_value_reader_unsafe(on9kvdb_value_reader reader, const value_reader_slot **slot_out) const;
    esp_err_t get_value_writer_unsafe(on9kvdb_value_writer writer, value_writer_slot **slot_out);
    esp_err_t open_value_unsafe(const value_view &view, on9kvdb_value_reader *reader_out);
    esp_err_t fill_reader_buffer_unsafe(value_reader_slot *reader);
    esp_err_t begin_value_write_unsafe(on9kvdb_transaction_handle transaction, on9kvdb_bytes key, uint32_t value_size,
                                       on9kvdb_value_writer *writer_out);
    esp_err_t write_value_unsafe(value_writer_slot *writer, const uint8_t *data, uint32_t size);
    esp_err_t finish_value_write_unsafe(value_writer_slot *writer);
    esp_err_t flush_value_writer_chunk_unsafe(value_writer_slot *writer, bool final_chunk);
    esp_err_t copy_external_value_unsafe(const on9kvdb_def::value_ref &source, uint32_t destination_bank,
                                         uint64_t destination_generation, uint32_t *destination_tail,
                                         on9kvdb_def::value_ref *destination_out);
    bool value_bank_is_pinned_unsafe(uint32_t bank_slot) const;
    bool value_bank_has_staged_reference_unsafe(uint32_t bank_slot) const;

private: // Handles and transactions
    esp_err_t get_handle_slot_unsafe(on9kvdb_handle handle, handle_slot **slot_out, uint16_t *slot_index_out = nullptr);
    esp_err_t get_handle_slot_unsafe(on9kvdb_handle handle, const handle_slot **slot_out,
                                     uint16_t *slot_index_out = nullptr) const;
    esp_err_t get_transaction_unsafe(on9kvdb_transaction_handle transaction, transaction_slot **slot_out);
    esp_err_t get_transaction_unsafe(on9kvdb_transaction_handle transaction, const transaction_slot **slot_out) const;
    void clear_transaction_unsafe();
    esp_err_t stage_value_unsafe(on9kvdb_transaction_handle transaction, on9kvdb_bytes key, const void *value, size_t value_size,
                                 uint8_t mutation_kind);
    esp_err_t stage_external_value_unsafe(on9kvdb_transaction_handle transaction, on9kvdb_bytes key,
                                          const on9kvdb_def::value_ref &reference);
    mutation_slot *find_staged_mutation_unsafe(transaction_slot *transaction, on9kvdb_bytes key);
    const mutation_slot *find_staged_mutation_unsafe(const transaction_slot *transaction, on9kvdb_bytes key) const;

private: // Manifest and provisioning
    esp_err_t setup_manifest();
    esp_err_t create_manifest();
    esp_err_t open_existing_manifest();
    esp_err_t load_manifest();
    esp_err_t write_manifest_copy(uint64_t generation, uint16_t state);
    esp_err_t stabilize_manifest_unsafe();
    esp_err_t provision_all_data_files();
    esp_err_t provision_one_data_file(on9kvdb_def::file_kind kind, uint32_t slot);
    esp_err_t load_file_identity(on9kvdb_def::file_kind kind, uint32_t slot,
                                 bool valid_copies[on9kvdb_def::identity_slot_count]) const;
    esp_err_t write_file_identity_copy(on9kvdb_def::file_kind kind, uint32_t slot, uint32_t copy_slot);

private: // WAL
    esp_err_t initialise_first_wal();
    esp_err_t ensure_wal_header(uint32_t slot, uint64_t generation, uint64_t first_transaction_sequence);
    esp_err_t load_wal_header(uint32_t slot, uint64_t expected_generation, on9kvdb_def::wal_header *header_out) const;
    esp_err_t recover_wal();
    esp_err_t scan_wal_slot(uint32_t slot, uint64_t generation, uint64_t *expected_sequence);
    esp_err_t find_later_wal_frame_unsafe(uint32_t slot, uint32_t start_offset, uint32_t region_end, uint64_t generation,
                                          uint64_t minimum_sequence, bool *found_out);
    esp_err_t rotate_wal_unsafe();
    esp_err_t append_transaction_unsafe(transaction_slot *transaction, const handle_slot &handle);
    esp_err_t calculate_transaction_payload_unsafe(const transaction_slot &transaction, const handle_slot &handle,
                                                   uint32_t *payload_size_out, uint32_t *checksum_out) const;
    esp_err_t copy_transaction_payload_unsafe(const transaction_slot &transaction, const handle_slot &handle,
                                              uint32_t stream_offset, uint8_t *destination, size_t destination_size) const;
    static void copy_wal_payload_segment_unsafe(wal_payload_copy_state *copy_state, const uint8_t *data, size_t size);
    esp_err_t parse_recovered_transaction_unsafe(const uint8_t *payload, size_t payload_size, uint16_t expected_mutation_count,
                                                 uint8_t namespace_name[on9kvdb_def::max_name_len], uint16_t *namespace_size_out);

private: // Immutable SSTables
    esp_err_t flush_memtable_unsafe();
    esp_err_t compact_tables_unsafe();
    esp_err_t load_compaction_cursor_unsafe(compaction_cursor *cursor, uint8_t *block_validation_buffer);
    esp_err_t advance_compaction_cursor_unsafe(compaction_cursor *cursor, uint8_t *block_validation_buffer);
    esp_err_t start_compaction_output_unsafe(compaction_output *output, uint32_t slot, uint64_t generation, uint8_t *data_block,
                                             uint8_t *index_block);
    esp_err_t append_compaction_entry_unsafe(compaction_output *output, const compaction_cursor *table_cursor,
                                             const memtable_record_header *memtable_record,
                                             const namespace_slot *memtable_namespace, uint32_t destination_value_bank,
                                             uint64_t destination_value_generation, uint32_t *destination_value_tail);
    esp_err_t finish_compaction_output_unsafe(compaction_output *output, on9kvdb_def::table_reference *reference_out);
    esp_err_t recover_tables_unsafe();
    esp_err_t validate_table_unsafe(const on9kvdb_def::table_reference &reference);
    esp_err_t cache_table_index_unsafe(const on9kvdb_def::table_reference &reference, const uint8_t *index_block);
    void invalidate_table_index_cache_unsafe(uint32_t slot);
    esp_err_t lookup_tables_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const;
    esp_err_t lookup_table_unsafe(const on9kvdb_def::table_reference &reference, const on9kvdb_def::composite_key &key,
                                  value_view *view_out) const;
    esp_err_t lookup_committed_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const;
    esp_err_t read_table_bytes_unsafe(uint32_t slot, uint64_t offset, uint8_t *destination, size_t size) const;
    esp_err_t write_table_bytes_unsafe(uint32_t slot, uint64_t offset, const uint8_t *source, size_t size);
    esp_err_t finish_table_data_block_unsafe(table_build_state *state);
    int compare_memtable_records_unsafe(uint32_t lhs_offset, uint32_t rhs_offset) const;
    void sift_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count, uint32_t root) const;
    void sort_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count) const;
    void reset_memtable_unsafe();

private: // Memtable and namespace registry
    esp_err_t find_namespace_unsafe(on9kvdb_bytes namespace_name, uint16_t *slot_index_out) const;
    esp_err_t ensure_namespace_capacity_unsafe(on9kvdb_bytes namespace_name, uint16_t *slot_index_out, bool publish);
    uint32_t hash_key_unsafe(uint16_t namespace_slot_index, const uint8_t *key, size_t key_size) const;
    esp_err_t find_memtable_bucket_unsafe(uint16_t namespace_slot_index, const uint8_t *key, size_t key_size,
                                          uint32_t *bucket_index_out, bool *found_out) const;
    esp_err_t lookup_memtable_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const;
    esp_err_t lookup_transaction_unsafe(const transaction_slot &transaction, const handle_slot &handle, on9kvdb_bytes key,
                                        value_view *view_out) const;
    esp_err_t preflight_memtable_transaction_unsafe(transaction_slot &transaction, on9kvdb_bytes namespace_name,
                                                    uint16_t *namespace_slot_out);
    void compact_memtable_unsafe();
    void remove_memtable_record_unsafe(uint32_t bucket_index);
    esp_err_t apply_transaction_to_memtable_unsafe(const transaction_slot &transaction, uint16_t namespace_slot_index,
                                                   uint64_t transaction_sequence);

private: // Raw block I/O and fixed logical regions
    esp_err_t get_storage_region_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t *offset_out,
                                        uint64_t *size_out) const;
    esp_err_t read_storage_bytes_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t offset, void *destination,
                                        size_t size) const;
    esp_err_t write_storage_bytes_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t offset, const void *source,
                                         size_t size);
    esp_err_t sync_storage_unsafe();
    esp_err_t storage_region_is_blank_unsafe(bool *blank_out) const;

private:
    static on9kvdb_def::storage_geometry get_build_geometry();
    static on9kvdb_def::logical_limits get_build_limits();

    on9kvdb_io *storage = nullptr;
    on9kvdb_cfg cfg = {};
    on9kvdb_def::manifest_record manifest = {};
    uint32_t manifest_slot = 0;
    uint32_t manifest_valid_copy_count = 0;
    bool manifest_stabilization_required = false;
    portMUX_TYPE lock_creation_mux = portMUX_INITIALIZER_UNLOCKED;
    SemaphoreHandle_t lifecycle_lock = nullptr;
    SemaphoreHandle_t operation_lock = nullptr;

    void *runtime_arena = nullptr;
    size_t runtime_arena_size = 0;
    uint8_t *io_frame = nullptr;
    uint8_t *io_bounce = nullptr;
    uint8_t *value_reader_buffers = nullptr;
    uint8_t *value_writer_buffer = nullptr;
    namespace_slot *namespaces = nullptr;
    handle_slot *handles = nullptr;
    transaction_slot *transaction = nullptr;
    memtable_bucket *memtable_index = nullptr;
    uint8_t *transaction_staging = nullptr;
    uint8_t *memtable_data = nullptr;
    table_index_cache_slot *table_index_cache = nullptr;
    uint8_t *table_lookup_value = nullptr;
    uint8_t *future_scratch = nullptr;
    size_t future_scratch_size = 0;
    value_reader_slot *value_readers = nullptr;
    value_writer_slot *value_writer = nullptr;
    uint32_t namespace_count = 0;
    uint32_t memtable_data_used = 0;
    uint32_t memtable_entry_count = 0;
    uint32_t wal_tail[on9kvdb_def::wal_file_count] = {};
    uint64_t next_transaction_sequence = 1;
    uint32_t handle_generation_counter = 1;
    uint32_t transaction_generation_counter = 1;
    on9kvdb_stats stats = {};
    bool initialized = false;
    bool shutting_down = false;
    bool storage_faulted = false;
    uint8_t value_bank_dirty_mask = 0;

private:
    static const constexpr char TAG[] = "on9kvdb";
};
