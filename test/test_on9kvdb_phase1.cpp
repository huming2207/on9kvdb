#include <cstring>
#include <type_traits>

#include <sdkconfig.h>
#include <unity.h>

#include "on9kvdb.hpp"
#include "on9kvdb_defs.hpp"

using get_u32_handle_signature = esp_err_t (on9kvdb::*)(on9kvdb_handle, const char *, uint32_t *) const;
using get_u32_transaction_signature = esp_err_t (on9kvdb::*)(on9kvdb_transaction_handle, const char *, uint32_t *) const;
using get_blob_handle_signature = esp_err_t (on9kvdb::*)(on9kvdb_handle, const char *, void *, size_t *) const;
using get_blob_transaction_signature = esp_err_t (on9kvdb::*)(on9kvdb_transaction_handle, const char *, void *, size_t *) const;

static_assert(std::is_same_v<decltype(static_cast<get_u32_handle_signature>(&on9kvdb::get_u32)), get_u32_handle_signature>);
static_assert(
    std::is_same_v<decltype(static_cast<get_u32_transaction_signature>(&on9kvdb::get_u32)), get_u32_transaction_signature>);
static_assert(std::is_same_v<decltype(static_cast<get_blob_handle_signature>(&on9kvdb::get_blob)), get_blob_handle_signature>);
static_assert(
    std::is_same_v<decltype(static_cast<get_blob_transaction_signature>(&on9kvdb::get_blob)), get_blob_transaction_signature>);

TEST_CASE("on9kvdb Phase 1 validates names", "[on9kvdb]")
{
    char maximum[on9kvdb_def::max_name_len + 1] = {};
    memset(maximum, 'x', on9kvdb_def::max_name_len);

    TEST_ASSERT_TRUE(on9kvdb_def::validate_name("key"));
    TEST_ASSERT_TRUE(on9kvdb_def::validate_name(maximum));
    TEST_ASSERT_FALSE(on9kvdb_def::validate_name(""));
}

TEST_CASE("on9kvdb Phase 1 file prefix detects corruption", "[on9kvdb]")
{
    uint8_t encoded[on9kvdb_def::file_prefix_size] = {};
    TEST_ASSERT_TRUE(on9kvdb_def::encode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::manifest_magic,
                                                     on9kvdb_def::file_kind::manifest, 0, 1, 4096));

    on9kvdb_def::decoded_file_prefix decoded = {};
    TEST_ASSERT_EQUAL(static_cast<int>(on9kvdb_def::format_status::ok),
                      static_cast<int>(on9kvdb_def::decode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::manifest_magic,
                                                                       on9kvdb_def::file_kind::manifest, &decoded)));

    encoded[20] ^= 0x01;
    TEST_ASSERT_EQUAL(static_cast<int>(on9kvdb_def::format_status::corrupt),
                      static_cast<int>(on9kvdb_def::decode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::manifest_magic,
                                                                       on9kvdb_def::file_kind::manifest, &decoded)));
}

TEST_CASE("on9kvdb Phase 2 manifest persists geometry", "[on9kvdb]")
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
    TEST_ASSERT_TRUE(on9kvdb_def::validate_storage_geometry(geometry));

    on9kvdb_def::manifest_record record = {};
    record.generation = 3;
    record.database_id = UINT64_C(0x1020304050607080);
    record.state = on9kvdb_def::manifest_state_ready;
    record.geometry = geometry;
    record.limits.wal_frame_bytes = on9kvdb_def::wal_frame_size;
    record.limits.max_namespaces = CONFIG_ON9KVDB_MAX_NAMESPACES;
    record.limits.max_open_handles = CONFIG_ON9KVDB_MAX_OPEN_HANDLES;
    record.limits.memtable_entries = CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT;
    record.limits.memtable_data_bytes = CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE;
    record.limits.max_transaction_mutations = CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS;
    record.limits.transaction_staging_bytes = CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE;
    record.active_wal_slot = 0;
    record.wal_generation[0] = 1;
    uint8_t encoded[on9kvdb_def::manifest_record_size] = {};
    TEST_ASSERT_TRUE(on9kvdb_def::encode_manifest_record(encoded, sizeof(encoded), record));

    on9kvdb_def::manifest_record decoded = {};
    TEST_ASSERT_EQUAL(static_cast<int>(on9kvdb_def::format_status::ok),
                      static_cast<int>(on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded)));
    TEST_ASSERT_EQUAL_UINT64(geometry.provisioned_size, decoded.geometry.provisioned_size);
    TEST_ASSERT_EQUAL_UINT32(geometry.table_count, decoded.geometry.table_count);
}

TEST_CASE("on9kvdb Phase 6 public diagnostics have bounded defaults", "[on9kvdb]")
{
    const on9kvdb_handle handle = {};
    const on9kvdb_transaction_handle transaction = {};
    const on9kvdb_stats stats = {};

    TEST_ASSERT_FALSE(handle.is_valid());
    TEST_ASSERT_FALSE(transaction.is_valid());
    TEST_ASSERT_EQUAL_UINT64(0, stats.logical_state_capacity_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, stats.wal_record_capacity_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, stats.runtime_memory_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_table_count);
    TEST_ASSERT_EQUAL_UINT32(0, stats.referenced_wal_count);
    TEST_ASSERT_FALSE(stats.manifest_stabilization_required);
    TEST_ASSERT_FALSE(stats.storage_faulted);
}
