#include <cstring>

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
