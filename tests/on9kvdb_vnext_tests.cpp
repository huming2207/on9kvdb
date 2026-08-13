#include <cstdio>
#include <cstring>

#include "on9kvdb_defs.hpp"

namespace
{
    int failures = 0;

    void check(bool condition, const char *message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", message);
            failures += 1;
        }
    }

    void test_binary_names()
    {
        const uint8_t namespace_name[] = {'n', 0, 's'};
        const uint8_t key_a[] = {'k', 0, 'a'};
        const uint8_t key_b[] = {'k', 0, 'b'};
        check(on9kvdb_def::validate_bytes(namespace_name, sizeof(namespace_name)), "embedded zero namespace is valid");
        check(on9kvdb_def::validate_bytes(key_a, sizeof(key_a)), "embedded zero key is valid");

        on9kvdb_def::composite_key left = {};
        left.namespace_size = sizeof(namespace_name);
        left.key_size = sizeof(key_a);
        memcpy(left.namespace_name, namespace_name, sizeof(namespace_name));
        memcpy(left.key, key_a, sizeof(key_a));
        on9kvdb_def::composite_key right = left;
        memcpy(right.key, key_b, sizeof(key_b));
        check(on9kvdb_def::compare_composite_key(left, right) < 0, "binary key ordering uses bytes after zero");
    }

    void test_external_table_record_crc()
    {
        uint8_t bytes[256] = {};
        const uint8_t namespace_name[] = {'n', 0, 's'};
        const uint8_t key[] = {'v', 0, '1'};
        on9kvdb_def::table_entry entry = {};
        entry.transaction_sequence = 7;
        entry.value_size = 9000;
        entry.namespace_size = sizeof(namespace_name);
        entry.key_size = sizeof(key);
        entry.flags = on9kvdb_def::table_entry_flag_external_value;
        entry.namespace_name = namespace_name;
        entry.key = key;
        entry.external_value.bank_slot = 1;
        entry.external_value.bank_generation = 3;
        entry.external_value.first_chunk_offset = on9kvdb_def::identity_region_size;
        entry.external_value.value_size = entry.value_size;
        entry.external_value.value_checksum = 0x12345678;
        size_t encoded_size = 0;
        check(on9kvdb_def::encode_table_entry(bytes, sizeof(bytes), 0, entry, &encoded_size), "encode external table record");
        on9kvdb_def::table_entry decoded = {};
        check(on9kvdb_def::decode_table_entry(bytes, sizeof(bytes), 0, &decoded) == on9kvdb_def::format_status::ok,
              "decode external table record");
        check(decoded.external_value.bank_slot == entry.external_value.bank_slot &&
                  decoded.external_value.value_checksum == entry.external_value.value_checksum,
              "external value descriptor round trips");
        bytes[encoded_size - 1U] ^= 1U;
        check(on9kvdb_def::decode_table_entry(bytes, sizeof(bytes), 0, &decoded) == on9kvdb_def::format_status::corrupt,
              "table record CRC catches corruption");
    }

    void test_in_place_value_chunk()
    {
        uint8_t chunk[on9kvdb_def::value_chunk_size] = {};
        uint8_t *payload = chunk + on9kvdb_def::value_chunk_header_size;
        for (uint32_t index = 0; index < 19; index += 1U) {
            payload[index] = static_cast<uint8_t>(index + 1U);
        }
        on9kvdb_def::value_chunk_header header = {};
        header.database_id = 1;
        header.bank_generation = 1;
        header.first_chunk_offset = on9kvdb_def::identity_region_size;
        header.value_size = 19;
        header.payload_size = 19;
        header.flags = on9kvdb_def::value_chunk_flag_final;
        check(on9kvdb_def::encode_value_chunk(chunk, sizeof(chunk), header, payload), "encode in-place value chunk");
        on9kvdb_def::value_chunk_header decoded = {};
        const uint8_t *decoded_payload = nullptr;
        check(on9kvdb_def::decode_value_chunk(chunk, sizeof(chunk), &decoded, &decoded_payload) == on9kvdb_def::format_status::ok,
              "decode value chunk");
        check(decoded_payload != nullptr && decoded_payload[0] == 1 && decoded_payload[18] == 19,
              "in-place payload remains intact");
    }

    void test_wide_handle_tokens()
    {
        check(on9kvdb_def::max_handle_generation > UINT32_MAX, "handle generation exceeds the old 23-bit lifetime");
        const uint64_t token = on9kvdb_def::make_handle_value(255, on9kvdb_def::max_handle_generation);
        uint16_t slot = 0;
        uint64_t generation = 0;
        check(token != 0 && on9kvdb_def::decode_handle_value(token, &slot, &generation) && slot == 255 &&
                  generation == on9kvdb_def::max_handle_generation,
              "maximum 55-bit handle generation round trips");
        check(on9kvdb_def::make_handle_value(256, 1) == 0 &&
                  on9kvdb_def::make_handle_value(0, on9kvdb_def::max_handle_generation + 1U) == 0,
              "wide handle encoding rejects out-of-range fields");
    }
}

int main()
{
    test_binary_names();
    test_external_table_record_crc();
    test_in_place_value_chunk();
    test_wide_handle_tokens();
    return failures == 0 ? 0 : 1;
}
