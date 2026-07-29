#include <cstring>

#include <sdkconfig.h>
#include <unity.h>

#include "on9kvdb_defs.hpp"

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
    TEST_ASSERT_TRUE(on9kvdb_def::encode_file_prefix(
        encoded, sizeof(encoded),
        on9kvdb_def::manifest_magic,
        on9kvdb_def::file_kind::manifest,
        0, 1, 4096));

    on9kvdb_def::decoded_file_prefix decoded = {};
    TEST_ASSERT_EQUAL(
        static_cast<int>(on9kvdb_def::format_status::ok),
        static_cast<int>(on9kvdb_def::decode_file_prefix(
            encoded, sizeof(encoded),
            on9kvdb_def::manifest_magic,
            on9kvdb_def::file_kind::manifest, &decoded)));

    encoded[20] ^= 0x01;
    TEST_ASSERT_EQUAL(
        static_cast<int>(on9kvdb_def::format_status::corrupt),
        static_cast<int>(on9kvdb_def::decode_file_prefix(
            encoded, sizeof(encoded),
            on9kvdb_def::manifest_magic,
            on9kvdb_def::file_kind::manifest, &decoded)));
}

TEST_CASE("on9kvdb Phase 2 manifest persists geometry", "[on9kvdb]")
{
    on9kvdb_def::storage_geometry geometry = {};
    geometry.provisioned_size =
        CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE;
    geometry.max_live_bytes =
        CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE;
    geometry.manifest_size = on9kvdb_def::manifest_file_size;
    geometry.wal_size = CONFIG_ON9KVDB_WAL_FILE_SIZE;
    geometry.wal_count = on9kvdb_def::wal_file_count;
    geometry.table_size = CONFIG_ON9KVDB_SSTABLE_FILE_SIZE;
    geometry.table_count = CONFIG_ON9KVDB_SSTABLE_COUNT;
    geometry.alignment = on9kvdb_def::format_alignment;
    TEST_ASSERT_TRUE(
        on9kvdb_def::validate_storage_geometry(geometry));

    on9kvdb_def::manifest_record record = {};
    record.generation = 3;
    record.database_id = UINT64_C(0x1020304050607080);
    record.state = on9kvdb_def::manifest_state_ready;
    record.geometry = geometry;
    uint8_t encoded[on9kvdb_def::manifest_record_size] = {};
    TEST_ASSERT_TRUE(on9kvdb_def::encode_manifest_record(
        encoded, sizeof(encoded), record));

    on9kvdb_def::manifest_record decoded = {};
    TEST_ASSERT_EQUAL(
        static_cast<int>(on9kvdb_def::format_status::ok),
        static_cast<int>(on9kvdb_def::decode_manifest_record(
            encoded, sizeof(encoded), &decoded)));
    TEST_ASSERT_EQUAL_UINT64(
        geometry.provisioned_size,
        decoded.geometry.provisioned_size);
    TEST_ASSERT_EQUAL_UINT32(
        geometry.table_count, decoded.geometry.table_count);
}
