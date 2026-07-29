#include <cstdint>
#include <cstdio>
#include <cstring>

#include "on9kvdb_defs.hpp"

namespace
{
    int failure_count = 0;

#define EXPECT_TRUE(condition)                                                                                                   \
    do {                                                                                                                         \
        if (!(condition)) {                                                                                                      \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, #condition);                             \
            failure_count += 1;                                                                                                  \
        }                                                                                                                        \
    } while (false)

#define EXPECT_EQ(expected, actual)                                                                                              \
    do {                                                                                                                         \
        const auto expected_value = (expected);                                                                                  \
        const auto actual_value = (actual);                                                                                      \
        if (expected_value != actual_value) {                                                                                    \
            std::fprintf(stderr, "%s:%d: values differ: %s, %s\n", __FILE__, __LINE__, #expected, #actual);                      \
            failure_count += 1;                                                                                                  \
        }                                                                                                                        \
    } while (false)

    on9kvdb_def::composite_key make_key(const char *namespace_name, const char *key)
    {
        on9kvdb_def::composite_key result = {};
        result.namespace_size = static_cast<uint8_t>(strlen(namespace_name));
        result.key_size = static_cast<uint8_t>(strlen(key));
        memcpy(result.namespace_name, namespace_name, result.namespace_size);
        memcpy(result.key, key, result.key_size);
        return result;
    }

    on9kvdb_def::storage_geometry make_geometry()
    {
        on9kvdb_def::storage_geometry geometry = {};
        geometry.provisioned_size = 4U * 1024U * 1024U;
        geometry.max_live_bytes = 768U * 1024U;
        geometry.manifest_size = on9kvdb_def::manifest_file_size;
        geometry.wal_size = 256U * 1024U;
        geometry.wal_count = on9kvdb_def::wal_file_count;
        geometry.table_size = 596U * 1024U;
        geometry.table_count = 6;
        geometry.alignment = on9kvdb_def::format_alignment;
        return geometry;
    }

    on9kvdb_def::logical_limits make_limits()
    {
        on9kvdb_def::logical_limits limits = {};
        limits.wal_frame_bytes = on9kvdb_def::wal_frame_size;
        limits.max_namespaces = 64;
        limits.max_open_handles = 8;
        limits.memtable_entries = 512;
        limits.memtable_data_bytes = 36U * 1024U;
        limits.max_transaction_mutations = 10;
        limits.transaction_staging_bytes = 24U * 1024U;
        limits.sstable_block_bytes = 12U * 1024U;
        return limits;
    }

    on9kvdb_def::table_metadata make_metadata()
    {
        on9kvdb_def::table_metadata metadata = {};
        metadata.database_id = UINT64_C(0x1122334455667788);
        metadata.generation = 3;
        metadata.min_sequence = 7;
        metadata.max_sequence = 11;
        metadata.slot = 2;
        metadata.block_size = 12U * 1024U;
        metadata.data_region_start = on9kvdb_def::table_data_region_offset;
        metadata.data_block_count = 2;
        metadata.index_offset = 580U * 1024U;
        metadata.footer_offset = 592U * 1024U;
        metadata.entry_count = 3;
        metadata.data_bytes = 144;
        metadata.content_checksum = UINT32_C(0xaabbccdd);
        metadata.min_key = make_key("config", "alpha");
        metadata.max_key = make_key("system", "zulu");
        return metadata;
    }

    bool select_manifest(const uint8_t slots[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size],
                         on9kvdb_def::manifest_record *selected_out)
    {
        bool found = false;
        on9kvdb_def::manifest_record selected = {};
        for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
            on9kvdb_def::manifest_record candidate = {};
            if (on9kvdb_def::decode_manifest_record(slots[slot], on9kvdb_def::manifest_record_size, &candidate) !=
                on9kvdb_def::format_status::ok) {
                continue;
            }
            if (!found || candidate.generation > selected.generation) {
                selected = candidate;
                found = true;
            }
        }
        if (found && selected_out != nullptr) {
            *selected_out = selected;
        }
        return found;
    }

    on9kvdb_def::manifest_record make_ready_manifest(uint64_t generation)
    {
        on9kvdb_def::manifest_record manifest = {};
        manifest.generation = generation;
        manifest.database_id = UINT64_C(0x1122334455667788);
        manifest.state = on9kvdb_def::manifest_state_ready;
        manifest.geometry = make_geometry();
        manifest.limits = make_limits();
        manifest.wal_generation[0] = 1;
        return manifest;
    }

    void attach_table_reference(on9kvdb_def::manifest_record *manifest, const on9kvdb_def::table_metadata &metadata)
    {
        on9kvdb_def::table_reference &reference = manifest->tables[metadata.slot];
        reference.active = true;
        reference.slot = metadata.slot;
        reference.data_block_count = metadata.data_block_count;
        reference.generation = metadata.generation;
        reference.min_sequence = metadata.min_sequence;
        reference.max_sequence = metadata.max_sequence;
        reference.entry_count = metadata.entry_count;
        reference.data_bytes = metadata.data_bytes;
        reference.content_checksum = metadata.content_checksum;
        reference.min_key = metadata.min_key;
        reference.max_key = metadata.max_key;
    }

    void test_manifest_table_reference()
    {
        on9kvdb_def::manifest_record manifest = make_ready_manifest(9);
        manifest.safe_checkpoint_sequence = 11;
        manifest.next_table_generation = 4;

        const on9kvdb_def::table_metadata metadata = make_metadata();
        attach_table_reference(&manifest, metadata);
        const on9kvdb_def::table_reference &reference = manifest.tables[metadata.slot];

        uint8_t encoded[on9kvdb_def::manifest_record_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(encoded, sizeof(encoded), manifest));
        on9kvdb_def::manifest_record decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok, on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded));
        EXPECT_TRUE(on9kvdb_def::table_reference_equal(reference, decoded.tables[metadata.slot]));

        encoded[on9kvdb_def::manifest_table_reference_offset + metadata.slot * on9kvdb_def::manifest_table_reference_size + 40] ^=
            1U;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt, on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded));
    }

    void test_manifest_bank_validation()
    {
        on9kvdb_def::manifest_record manifest = make_ready_manifest(4);
        manifest.safe_checkpoint_sequence = 11;
        manifest.next_table_generation = 4;
        attach_table_reference(&manifest, make_metadata());

        uint8_t encoded[on9kvdb_def::manifest_record_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(encoded, sizeof(encoded), manifest));
        on9kvdb_def::manifest_record decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok, on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded));
        EXPECT_EQ(UINT32_C(0), decoded.active_table_bank);

        manifest.active_table_bank = 1;
        EXPECT_TRUE(!on9kvdb_def::encode_manifest_record(encoded, sizeof(encoded), manifest));
    }

    void test_interrupted_table_publication()
    {
        on9kvdb_def::manifest_record base = make_ready_manifest(3);
        on9kvdb_def::table_metadata metadata = make_metadata();
        metadata.slot = 3;
        metadata.generation = 1;
        on9kvdb_def::manifest_record published = base;
        published.generation = 4;
        published.active_table_bank = 1;
        published.next_table_generation = 2;
        published.safe_checkpoint_sequence = metadata.max_sequence;
        attach_table_reference(&published, metadata);
        on9kvdb_def::manifest_record stabilized = published;
        stabilized.generation = 5;

        uint8_t base_encoded[on9kvdb_def::manifest_record_size] = {};
        uint8_t published_encoded[on9kvdb_def::manifest_record_size] = {};
        uint8_t stabilized_encoded[on9kvdb_def::manifest_record_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(base_encoded, sizeof(base_encoded), base));
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(published_encoded, sizeof(published_encoded), published));
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(stabilized_encoded, sizeof(stabilized_encoded), stabilized));

        for (uint32_t stop_after = 0; stop_after < 3; stop_after += 1) {
            uint8_t durable[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size] = {};
            memcpy(durable[0], base_encoded, sizeof(base_encoded));

            if (stop_after >= 1) {
                memcpy(durable[1], published_encoded, sizeof(published_encoded));
            }
            if (stop_after >= 2) {
                memcpy(durable[0], stabilized_encoded, sizeof(stabilized_encoded));
            }

            on9kvdb_def::manifest_record selected = {};
            EXPECT_TRUE(select_manifest(durable, &selected));
            if (selected.tables[metadata.slot].active) {
                EXPECT_EQ(UINT32_C(1), selected.active_table_bank);
                EXPECT_TRUE(selected.generation == published.generation || selected.generation == stabilized.generation);
            } else {
                EXPECT_EQ(base.generation, selected.generation);
            }
        }
    }

    void test_table_metadata()
    {
        const on9kvdb_def::table_metadata metadata = make_metadata();
        uint8_t encoded[on9kvdb_def::table_metadata_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_table_metadata(encoded, sizeof(encoded), on9kvdb_def::table_header_magic, metadata));

        on9kvdb_def::table_metadata decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_table_metadata(encoded, sizeof(encoded), on9kvdb_def::table_header_magic, &decoded));
        EXPECT_TRUE(on9kvdb_def::table_metadata_equal(metadata, decoded));

        encoded[56] ^= 1U;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt,
                  on9kvdb_def::decode_table_metadata(encoded, sizeof(encoded), on9kvdb_def::table_header_magic, &decoded));
    }

    void test_data_and_index_blocks()
    {
        static const constexpr size_t block_size = 12U * 1024U;
        uint8_t data_block[block_size] = {};
        static const constexpr uint8_t value[] = {0x78, 0x56, 0x34, 0x12};
        const char namespace_name[] = "config";
        const char key[] = "counter";

        on9kvdb_def::table_entry entry = {};
        entry.transaction_sequence = 11;
        entry.value_size = sizeof(value);
        entry.namespace_size = sizeof(namespace_name) - 1U;
        entry.key_size = sizeof(key) - 1U;
        entry.type = 0x06;
        entry.namespace_name = reinterpret_cast<const uint8_t *>(namespace_name);
        entry.key = reinterpret_cast<const uint8_t *>(key);
        entry.value = value;
        size_t entry_size = 0;
        EXPECT_TRUE(on9kvdb_def::encode_table_entry(data_block, sizeof(data_block), on9kvdb_def::table_block_header_size, entry,
                                                    &entry_size));

        on9kvdb_def::table_block_header block_header = {};
        block_header.generation = 3;
        block_header.block_index = 0;
        block_header.entry_count = 1;
        block_header.payload_size = entry_size;
        EXPECT_TRUE(on9kvdb_def::encode_table_block_header(data_block, sizeof(data_block), block_header));

        on9kvdb_def::table_block_header decoded_header = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_table_block_header(data_block, sizeof(data_block), &decoded_header));
        on9kvdb_def::table_entry decoded_entry = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_table_entry(data_block, sizeof(data_block), on9kvdb_def::table_block_header_size,
                                                  &decoded_entry));
        EXPECT_EQ(entry.transaction_sequence, decoded_entry.transaction_sequence);
        EXPECT_EQ(0, memcmp(value, decoded_entry.value, sizeof(value)));

        uint8_t index_block[block_size] = {};
        on9kvdb_def::table_index_entry index_entry = {};
        index_entry.first_sequence = entry.transaction_sequence;
        index_entry.block_offset = on9kvdb_def::table_data_region_offset;
        index_entry.namespace_size = entry.namespace_size;
        index_entry.key_size = entry.key_size;
        index_entry.namespace_name = entry.namespace_name;
        index_entry.key = entry.key;
        size_t index_entry_size = 0;
        EXPECT_TRUE(on9kvdb_def::encode_table_index_entry(index_block, sizeof(index_block), on9kvdb_def::table_index_header_size,
                                                          index_entry, &index_entry_size));

        on9kvdb_def::table_index_header index_header = {};
        index_header.generation = 3;
        index_header.entry_count = 1;
        index_header.payload_size = index_entry_size;
        index_header.data_block_count = 1;
        EXPECT_TRUE(on9kvdb_def::encode_table_index_header(index_block, sizeof(index_block), index_header));
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_table_index_header(index_block, sizeof(index_block), &index_header));

        data_block[on9kvdb_def::table_block_header_size + entry_size] = 1;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt,
                  on9kvdb_def::decode_table_block_header(data_block, sizeof(data_block), &decoded_header));
    }
}

int main()
{
    test_manifest_table_reference();
    test_manifest_bank_validation();
    test_interrupted_table_publication();
    test_table_metadata();
    test_data_and_index_blocks();

    if (failure_count != 0) {
        std::fprintf(stderr, "%d Phase 4 test(s) failed\n", failure_count);
        return 1;
    }

    std::puts("on9kvdb Phase 4 format tests passed");
    return 0;
}
