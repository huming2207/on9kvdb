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

    on9kvdb_def::storage_geometry make_geometry(uint64_t logical_state_bytes)
    {
        on9kvdb_def::storage_geometry geometry = {};
        geometry.provisioned_size = 4U * 1024U * 1024U;
        geometry.max_live_bytes = logical_state_bytes;
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

    on9kvdb_def::composite_key make_key(const char *key)
    {
        on9kvdb_def::composite_key result = {};
        static const constexpr char namespace_name[] = "config";
        result.namespace_size = sizeof(namespace_name) - 1U;
        result.key_size = static_cast<uint8_t>(strlen(key));
        memcpy(result.namespace_name, namespace_name, result.namespace_size);
        memcpy(result.key, key, result.key_size);
        return result;
    }

    on9kvdb_def::table_reference make_reference(uint32_t slot, uint64_t generation, uint64_t sequence, const char *key)
    {
        on9kvdb_def::table_reference reference = {};
        reference.active = true;
        reference.slot = slot;
        reference.generation = generation;
        reference.min_sequence = sequence;
        reference.max_sequence = sequence;
        reference.entry_count = 1;
        reference.data_block_count = 1;
        reference.data_bytes = 40;
        reference.content_checksum = UINT32_C(0x12345678);
        reference.min_key = make_key(key);
        reference.max_key = reference.min_key;
        return reference;
    }

    on9kvdb_def::manifest_record make_base_manifest()
    {
        on9kvdb_def::manifest_record manifest = {};
        manifest.generation = 5;
        manifest.database_id = UINT64_C(0x1122334455667788);
        manifest.state = on9kvdb_def::manifest_state_ready;
        manifest.geometry = make_geometry(768U * 1024U);
        manifest.limits = make_limits();
        manifest.active_wal_slot = 1;
        manifest.wal_generation[0] = 1;
        manifest.wal_generation[1] = 2;
        manifest.safe_checkpoint_sequence = 5;
        manifest.next_table_generation = 2;
        manifest.tables[0] = make_reference(0, 1, 5, "alpha");
        return manifest;
    }

    bool encode_manifest(const on9kvdb_def::manifest_record &manifest, uint8_t encoded[on9kvdb_def::manifest_record_size])
    {
        return on9kvdb_def::encode_manifest_record(encoded, on9kvdb_def::manifest_record_size, manifest);
    }

    bool bank_is_unreferenced(const uint8_t copies[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size],
                              uint32_t bank)
    {
        for (uint32_t copy = 0; copy < on9kvdb_def::manifest_slot_count; copy += 1) {
            on9kvdb_def::manifest_record decoded = {};
            if (on9kvdb_def::decode_manifest_record(copies[copy], on9kvdb_def::manifest_record_size, &decoded) ==
                    on9kvdb_def::format_status::ok &&
                decoded.active_table_bank == bank) {
                return false;
            }
        }
        return true;
    }

    bool wal_is_unreferenced(const uint8_t copies[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size],
                             uint32_t wal_slot)
    {
        for (uint32_t copy = 0; copy < on9kvdb_def::manifest_slot_count; copy += 1) {
            on9kvdb_def::manifest_record decoded = {};
            if (on9kvdb_def::decode_manifest_record(copies[copy], on9kvdb_def::manifest_record_size, &decoded) ==
                    on9kvdb_def::format_status::ok &&
                decoded.wal_generation[wal_slot] != 0) {
                return false;
            }
        }
        return true;
    }

    uint32_t next_fit_block_count(uint64_t target_bytes)
    {
        static const constexpr uint32_t payload_bytes = 12288U - on9kvdb_def::table_block_header_size;
        static const constexpr uint32_t sizes[] = {3952, 8280};
        uint64_t emitted = 0;
        uint32_t used = 0;
        uint32_t blocks = 1;
        uint32_t index = 0;
        while (emitted < target_bytes) {
            uint32_t size = sizes[index & 1U];
            if (size > target_bytes - emitted) {
                size = static_cast<uint32_t>(target_bytes - emitted);
                size = (size + 7U) & ~UINT32_C(7);
            }
            if (used > 0 && size > payload_bytes - used) {
                blocks += 1U;
                used = 0;
            }
            used += size;
            emitted += size;
            index += 1U;
        }
        return blocks;
    }

    void test_capacity_contract()
    {
        const on9kvdb_def::logical_limits limits = make_limits();
        EXPECT_TRUE(on9kvdb_def::validate_compaction_capacity(make_geometry(768U * 1024U), limits));
        EXPECT_TRUE(!on9kvdb_def::validate_compaction_capacity(make_geometry(1024U * 1024U), limits));

        on9kvdb_def::storage_geometry odd = make_geometry(512U * 1024U);
        odd.table_count = 5;
        odd.provisioned_size = odd.manifest_size + static_cast<uint64_t>(odd.wal_count) * odd.wal_size +
                               static_cast<uint64_t>(odd.table_count) * odd.table_size;
        EXPECT_TRUE(!on9kvdb_def::validate_compaction_capacity(odd, limits));

        on9kvdb_def::storage_geometry short_wal = make_geometry(512U * 1024U);
        short_wal.wal_size = on9kvdb_def::wal_record_region_offset;
        short_wal.provisioned_size = short_wal.manifest_size + static_cast<uint64_t>(short_wal.wal_count) * short_wal.wal_size +
                                     static_cast<uint64_t>(short_wal.table_count) * short_wal.table_size;
        EXPECT_TRUE(!on9kvdb_def::validate_storage_geometry(short_wal));
        short_wal.wal_size += on9kvdb_def::wal_frame_size;
        short_wal.provisioned_size = short_wal.manifest_size + static_cast<uint64_t>(short_wal.wal_count) * short_wal.wal_size +
                                     static_cast<uint64_t>(short_wal.table_count) * short_wal.table_size;
        EXPECT_TRUE(on9kvdb_def::validate_storage_geometry(short_wal));

        on9kvdb_def::storage_geometry excessive_index = make_geometry(64U * 1024U);
        static const constexpr uint32_t data_block_count = 1000;
        excessive_index.table_size = on9kvdb_def::table_data_region_offset +
                                     (data_block_count + 1U) * limits.sstable_block_bytes + on9kvdb_def::table_footer_slot_size;
        excessive_index.provisioned_size = excessive_index.manifest_size +
                                           static_cast<uint64_t>(excessive_index.wal_count) * excessive_index.wal_size +
                                           static_cast<uint64_t>(excessive_index.table_count) * excessive_index.table_size;
        EXPECT_TRUE(on9kvdb_def::validate_storage_geometry(excessive_index));
        EXPECT_TRUE(on9kvdb_def::validate_logical_limits(limits));
        EXPECT_TRUE(!on9kvdb_def::validate_compaction_capacity(excessive_index, limits));

        EXPECT_TRUE(next_fit_block_count(768U * 1024U) <= 3U * 47U);
        EXPECT_TRUE(next_fit_block_count(1024U * 1024U) > 3U * 47U);
    }

    void test_stabilized_reuse_boundary()
    {
        const on9kvdb_def::manifest_record base = make_base_manifest();
        on9kvdb_def::manifest_record published = base;
        published.generation = 6;
        published.active_table_bank = 1;
        published.safe_checkpoint_sequence = 9;
        published.next_table_generation = 3;
        published.wal_generation[0] = 0;
        published.tables[0] = {};
        published.tables[3] = make_reference(3, 2, 9, "alpha");
        on9kvdb_def::manifest_record stabilized = published;
        stabilized.generation = 7;

        uint8_t base_encoded[on9kvdb_def::manifest_record_size] = {};
        uint8_t published_encoded[on9kvdb_def::manifest_record_size] = {};
        uint8_t stabilized_encoded[on9kvdb_def::manifest_record_size] = {};
        EXPECT_TRUE(encode_manifest(base, base_encoded));
        EXPECT_TRUE(encode_manifest(published, published_encoded));
        EXPECT_TRUE(encode_manifest(stabilized, stabilized_encoded));

        uint8_t copies[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size] = {};
        memcpy(copies[0], base_encoded, sizeof(base_encoded));
        memcpy(copies[1], published_encoded, sizeof(published_encoded));
        EXPECT_TRUE(!bank_is_unreferenced(copies, 0));
        EXPECT_TRUE(!wal_is_unreferenced(copies, 0));

        memcpy(copies[0], stabilized_encoded, sizeof(stabilized_encoded));
        EXPECT_TRUE(bank_is_unreferenced(copies, 0));
        EXPECT_TRUE(wal_is_unreferenced(copies, 0));
    }
}

int main()
{
    test_capacity_contract();
    test_stabilized_reuse_boundary();

    if (failure_count != 0) {
        std::fprintf(stderr, "%d Phase 5 test(s) failed\n", failure_count);
        return 1;
    }

    std::puts("on9kvdb Phase 5 policy tests passed");
    return 0;
}
