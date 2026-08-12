#pragma once

#include <cstddef>
#include <cstdint>

#ifndef CONFIG_ON9KVDB_INLINE_VALUE_SIZE
#define CONFIG_ON9KVDB_INLINE_VALUE_SIZE 1024
#endif

#define ON9KVDB_PACKED __attribute__((packed, aligned(1)))

namespace on9kvdb_def
{
    static const constexpr size_t max_name_len = 128;
    static const constexpr uint32_t max_value_len = UINT32_MAX - 1U;
    static const constexpr uint32_t inline_value_len = CONFIG_ON9KVDB_INLINE_VALUE_SIZE;
    static const constexpr size_t max_transaction_mutations = 10;
    static const constexpr size_t runtime_memory_budget_default = 100U * 1024U;
    static const constexpr size_t runtime_memory_budget_max = 200U * 1024U - 1U;
    static const constexpr uint32_t handle_slot_capacity = 256;
    static const constexpr uint32_t handle_slot_bits = 9;
    static const constexpr uint32_t handle_slot_mask = (UINT32_C(1) << handle_slot_bits) - 1U;
    static const constexpr uint32_t max_handle_generation = UINT32_MAX >> handle_slot_bits;
    static const constexpr uint64_t max_fat32_file_size = UINT32_MAX;
    static const constexpr uint32_t format_alignment = 4096;
    static const constexpr uint32_t manifest_slot_size = format_alignment;
    static const constexpr uint32_t manifest_slot_count = 2;
    static const constexpr uint32_t manifest_file_size = manifest_slot_size * manifest_slot_count;
    static const constexpr uint32_t identity_slot_size = format_alignment;
    static const constexpr uint32_t identity_slot_count = 2;
    static const constexpr uint32_t identity_region_size = identity_slot_size * identity_slot_count;
    static const constexpr uint32_t wal_file_count = 2;
    static const constexpr uint32_t value_bank_count = 2;
    static const constexpr uint32_t wal_header_slot_size = format_alignment;
    static const constexpr uint32_t wal_header_slot_count = 2;
    static const constexpr uint32_t wal_header_region_offset = identity_region_size;
    static const constexpr uint32_t wal_header_region_size = wal_header_slot_size * wal_header_slot_count;
    static const constexpr uint32_t wal_record_region_offset = wal_header_region_offset + wal_header_region_size;
    static const constexpr uint32_t wal_frame_size = format_alignment;
    static const constexpr uint32_t wal_frame_header_size = 64;
    static const constexpr uint32_t wal_frame_payload_capacity = wal_frame_size - wal_frame_header_size;
    static const constexpr uint32_t max_table_count = 16;
    static const constexpr uint32_t table_header_slot_size = format_alignment;
    static const constexpr uint32_t table_header_slot_count = 2;
    static const constexpr uint32_t table_header_region_offset = identity_region_size;
    static const constexpr uint32_t table_header_region_size = table_header_slot_size * table_header_slot_count;
    static const constexpr uint32_t table_data_region_offset = table_header_region_offset + table_header_region_size;
    static const constexpr uint32_t table_footer_slot_size = format_alignment;
    static const constexpr uint32_t table_block_header_size = 64;
    static const constexpr uint32_t table_index_header_size = 64;
    static const constexpr uint32_t table_entry_header_size = 24;
    static const constexpr uint32_t value_ref_encoded_size = 24;
    static const constexpr uint32_t table_index_entry_header_size = 16;
    static const constexpr uint32_t value_chunk_size = format_alignment;
    static const constexpr uint32_t value_chunk_header_size = 64;
    static const constexpr uint32_t value_chunk_payload_size = value_chunk_size - value_chunk_header_size;

    static const constexpr uint16_t storage_revision = 6;
    static const constexpr uint16_t geometry_revision = 2;
    static const constexpr uint16_t logical_limits_revision = 3;
    static const constexpr uint16_t wal_header_revision = 1;
    static const constexpr uint16_t wal_frame_revision = 1;
    static const constexpr uint16_t table_header_revision = 1;
    static const constexpr uint16_t table_block_revision = 1;
    static const constexpr uint16_t table_index_revision = 1;
    static const constexpr uint16_t table_footer_revision = 1;
    static const constexpr uint16_t value_chunk_revision = 1;
    static const constexpr uint32_t manifest_magic = 0x394d564bUL;     // "KVM9"
    static const constexpr uint32_t wal_magic = 0x3957564bUL;          // "KVW9"
    static const constexpr uint32_t table_magic = 0x3954564bUL;        // "KVT9"
    static const constexpr uint32_t wal_header_magic = 0x3948574bUL;   // "KWH9"
    static const constexpr uint32_t wal_frame_magic = 0x3946574bUL;    // "KWF9"
    static const constexpr uint32_t table_header_magic = 0x3948544bUL; // "KTH9"
    static const constexpr uint32_t table_block_magic = 0x3942544bUL;  // "KTB9"
    static const constexpr uint32_t table_index_magic = 0x3949544bUL;  // "KTI9"
    static const constexpr uint32_t table_footer_magic = 0x3946544bUL; // "KTF9"
    static const constexpr uint32_t value_bank_magic = 0x3956424bUL;   // "KBV9"
    static const constexpr uint32_t value_chunk_magic = 0x3956434bUL;  // "KCV9"
    static const constexpr uint16_t manifest_state_provisioning_owned = 0x7001;
    static const constexpr uint16_t manifest_state_ready = 0x7002;
    static const constexpr uint16_t file_prefix_flag_identity = 1U << 0;
    static const constexpr uint16_t wal_header_state_active = 0x7101;
    static const constexpr uint16_t wal_frame_flag_commit = 1U << 0;
    static const constexpr uint8_t table_reference_flag_active = 1U << 0;
    static const constexpr uint8_t table_entry_flag_tombstone = 1U << 0;
    static const constexpr uint8_t table_entry_flag_external_value = 1U << 1;
    static const constexpr uint16_t value_chunk_flag_final = 1U << 0;
    static const constexpr uint8_t memtable_flag_tombstone = 1U << 0;
    static const constexpr uint8_t memtable_flag_external_value = 1U << 1;
    static const constexpr uint8_t mutation_kind_set = 1;
    static const constexpr uint8_t mutation_kind_tombstone = 2;

    enum class file_kind : uint16_t {
        invalid = 0,
        manifest = 1,
        wal = 2,
        table = 3,
        value_bank = 4,
    };

    enum class format_status : uint8_t {
        ok = 0,
        invalid_argument,
        invalid_size,
        invalid_magic,
        invalid_revision,
        new_version,
        invalid_kind,
        invalid_generation,
        corrupt,
    };

    struct ON9KVDB_PACKED file_prefix {
        uint32_t magic;
        uint16_t revision;
        uint16_t size;
        uint16_t kind;
        uint16_t flags;
        uint64_t generation;
        uint32_t payload_size;
        uint32_t checksum;
    };

    static_assert(sizeof(file_prefix) == 28);
    static_assert(offsetof(file_prefix, generation) == 12);
    static_assert(offsetof(file_prefix, checksum) == 24);

    static const constexpr size_t file_prefix_size = sizeof(file_prefix);
    static const constexpr size_t file_prefix_checksum_offset = offsetof(file_prefix, checksum);

    struct decoded_file_prefix {
        uint32_t magic = 0;
        uint16_t revision = 0;
        file_kind kind = file_kind::invalid;
        uint16_t flags = 0;
        uint64_t generation = 0;
        uint32_t payload_size = 0;
        uint32_t checksum = 0;
    };

    struct storage_geometry {
        uint64_t provisioned_size = 0;
        uint64_t max_live_bytes = 0;
        uint32_t manifest_size = 0;
        uint32_t wal_size = 0;
        uint32_t wal_count = 0;
        uint32_t table_size = 0;
        uint32_t table_count = 0;
        uint32_t value_bank_size = 0;
        uint32_t value_bank_count = 0;
        uint32_t alignment = 0;
    };

    struct logical_limits {
        uint32_t wal_frame_bytes = 0;
        uint32_t max_namespaces = 0;
        uint32_t max_open_handles = 0;
        uint32_t memtable_entries = 0;
        uint32_t memtable_data_bytes = 0;
        uint32_t max_transaction_mutations = 0;
        uint32_t transaction_staging_bytes = 0;
        uint32_t sstable_block_bytes = 0;
        uint32_t inline_value_bytes = 0;
    };

    struct composite_key {
        uint16_t namespace_size = 0;
        uint16_t key_size = 0;
        uint8_t namespace_name[max_name_len] = {};
        uint8_t key[max_name_len] = {};
    };

    struct value_ref {
        uint64_t bank_generation = 0;
        uint32_t first_chunk_offset = 0;
        uint32_t value_size = 0;
        uint32_t value_checksum = 0;
        uint8_t bank_slot = UINT8_MAX;
    };

    struct table_reference {
        bool active = false;
        uint8_t level = 0;
        uint32_t slot = 0;
        uint32_t data_block_count = 0;
        uint64_t generation = 0;
        uint64_t min_sequence = 0;
        uint64_t max_sequence = 0;
        uint32_t entry_count = 0;
        uint32_t data_bytes = 0;
        uint32_t content_checksum = 0;
        composite_key min_key = {};
        composite_key max_key = {};
    };

    struct manifest_record {
        uint64_t generation = 0;
        uint64_t database_id = 0;
        uint16_t state = 0;
        storage_geometry geometry = {};
        logical_limits limits = {};
        uint32_t active_wal_slot = 0;
        uint64_t wal_generation[wal_file_count] = {};
        uint64_t safe_checkpoint_sequence = 0;
        uint64_t next_table_generation = 1;
        uint32_t active_table_bank = 0;
        uint32_t active_value_bank = 0;
        uint64_t value_bank_generation[value_bank_count] = {};
        uint32_t value_bank_tail[value_bank_count] = {};
        table_reference tables[max_table_count] = {};
    };

    struct file_identity {
        uint64_t generation = 0;
        uint64_t database_id = 0;
        uint64_t file_size = 0;
        uint32_t slot = 0;
        file_kind kind = file_kind::invalid;
    };

    struct wal_header {
        uint64_t database_id = 0;
        uint64_t generation = 0;
        uint64_t first_transaction_sequence = 0;
        uint32_t slot = 0;
        uint32_t record_region_start = 0;
        uint32_t record_region_end = 0;
        uint32_t frame_size = 0;
        uint16_t state = 0;
    };

    struct wal_frame_header {
        uint64_t database_id = 0;
        uint64_t wal_generation = 0;
        uint64_t transaction_sequence = 0;
        uint16_t frame_index = 0;
        uint16_t frame_count = 0;
        uint16_t mutation_count = 0;
        uint16_t flags = 0;
        uint32_t payload_size = 0;
        uint32_t transaction_payload_size = 0;
        uint32_t transaction_checksum = 0;
        uint32_t payload_checksum = 0;
    };

    struct table_metadata {
        uint64_t database_id = 0;
        uint64_t generation = 0;
        uint64_t min_sequence = 0;
        uint64_t max_sequence = 0;
        uint32_t slot = 0;
        uint32_t block_size = 0;
        uint32_t data_region_start = 0;
        uint32_t data_block_count = 0;
        uint32_t index_offset = 0;
        uint32_t footer_offset = 0;
        uint32_t entry_count = 0;
        uint32_t data_bytes = 0;
        uint32_t content_checksum = 0;
        uint8_t level = 0;
        composite_key min_key = {};
        composite_key max_key = {};
    };

    struct table_block_header {
        uint64_t generation = 0;
        uint32_t block_index = 0;
        uint16_t entry_count = 0;
        uint32_t payload_size = 0;
    };

    struct table_index_header {
        uint64_t generation = 0;
        uint32_t entry_count = 0;
        uint32_t payload_size = 0;
        uint32_t data_block_count = 0;
    };

    struct table_entry {
        uint64_t transaction_sequence = 0;
        uint32_t total_size = 0;
        uint32_t value_size = 0;
        uint8_t namespace_size = 0;
        uint8_t key_size = 0;
        uint8_t reserved0 = 0;
        uint8_t flags = 0;
        value_ref external_value = {};
        const uint8_t *namespace_name = nullptr;
        const uint8_t *key = nullptr;
        const uint8_t *value = nullptr;
    };

    struct table_index_entry {
        uint64_t first_sequence = 0;
        uint32_t block_offset = 0;
        uint16_t total_size = 0;
        uint8_t namespace_size = 0;
        uint8_t key_size = 0;
        const uint8_t *namespace_name = nullptr;
        const uint8_t *key = nullptr;
    };

    struct value_chunk_header {
        uint64_t database_id = 0;
        uint64_t bank_generation = 0;
        uint32_t first_chunk_offset = 0;
        uint32_t value_size = 0;
        uint32_t value_offset = 0;
        uint16_t payload_size = 0;
        uint16_t flags = 0;
        uint32_t payload_checksum = 0;
    };

    static const constexpr size_t manifest_table_reference_size = 56;
    static const constexpr size_t manifest_table_reference_offset = 224;
    static const constexpr size_t manifest_record_size = manifest_slot_size;
    static const constexpr size_t manifest_record_checksum_offset = manifest_record_size - sizeof(uint32_t);
    static const constexpr size_t file_identity_size = 56;
    static const constexpr size_t file_identity_checksum_offset = 52;
    static const constexpr size_t wal_header_size = 64;
    static const constexpr size_t wal_header_checksum_offset = 60;
    static const constexpr size_t wal_frame_checksum_offset = 60;
    static const constexpr size_t table_metadata_size = 640;
    static const constexpr size_t table_metadata_checksum_offset = 636;
    static const constexpr size_t table_block_checksum_offset = 60;
    static const constexpr size_t table_index_checksum_offset = 60;

    static_assert(wal_frame_header_size == 64);
    static_assert(wal_frame_payload_capacity == 4032);
    static_assert(manifest_table_reference_offset + max_table_count * manifest_table_reference_size <
                  manifest_record_checksum_offset);

    bool checked_add_size(size_t lhs, size_t rhs, size_t *result_out);
    bool checked_mul_size(size_t lhs, size_t rhs, size_t *result_out);
    bool checked_align_up_size(size_t value, size_t alignment, size_t *result_out);
    bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out);
    bool checked_mul_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out);

    bool write_u16_le(uint8_t *buf, size_t buf_len, size_t offset, uint16_t value);
    bool write_u32_le(uint8_t *buf, size_t buf_len, size_t offset, uint32_t value);
    bool write_u64_le(uint8_t *buf, size_t buf_len, size_t offset, uint64_t value);
    bool read_u16_le(const uint8_t *buf, size_t buf_len, size_t offset, uint16_t *value_out);
    bool read_u32_le(const uint8_t *buf, size_t buf_len, size_t offset, uint32_t *value_out);
    bool read_u64_le(const uint8_t *buf, size_t buf_len, size_t offset, uint64_t *value_out);

    uint32_t calc_crc32(const uint8_t *buf, size_t len);
    uint32_t calc_crc32_update(uint32_t crc, const uint8_t *buf, size_t len);

    bool validate_bytes(const uint8_t *data, uint16_t size, uint16_t maximum_size = max_name_len);

    uint32_t make_handle_value(uint16_t slot, uint32_t generation);
    bool decode_handle_value(uint32_t value, uint16_t *slot_out, uint32_t *generation_out);
    bool is_handle_value(uint32_t value, uint16_t slot, uint32_t generation);

    bool encode_file_prefix(uint8_t *buf, size_t buf_len, uint32_t magic, file_kind kind, uint16_t flags, uint64_t generation,
                            uint32_t payload_size);
    format_status decode_file_prefix(const uint8_t *buf, size_t buf_len, uint32_t expected_magic, file_kind expected_kind,
                                     decoded_file_prefix *prefix_out);

    bool validate_storage_geometry(const storage_geometry &geometry);
    bool storage_geometry_equal(const storage_geometry &lhs, const storage_geometry &rhs);
    bool validate_logical_limits(const logical_limits &limits);
    bool validate_compaction_capacity(const storage_geometry &geometry, const logical_limits &limits);
    bool logical_limits_equal(const logical_limits &lhs, const logical_limits &rhs);
    bool encode_manifest_record(uint8_t *buf, size_t buf_len, const manifest_record &record);
    format_status decode_manifest_record(const uint8_t *buf, size_t buf_len, manifest_record *record_out);
    bool encode_file_identity(uint8_t *buf, size_t buf_len, const file_identity &identity);
    format_status decode_file_identity(const uint8_t *buf, size_t buf_len, file_kind expected_kind, file_identity *identity_out);
    bool encode_wal_header(uint8_t *buf, size_t buf_len, const wal_header &header);
    format_status decode_wal_header(const uint8_t *buf, size_t buf_len, wal_header *header_out);
    bool wal_header_equal(const wal_header &lhs, const wal_header &rhs);
    bool encode_wal_frame(uint8_t *frame, size_t frame_len, const wal_frame_header &header, const uint8_t *payload,
                          size_t payload_len);
    format_status decode_wal_frame(const uint8_t *frame, size_t frame_len, wal_frame_header *header_out,
                                   const uint8_t **payload_out);
    bool composite_key_equal(const composite_key &lhs, const composite_key &rhs);
    int compare_composite_key(const composite_key &lhs, const composite_key &rhs);
    bool table_reference_equal(const table_reference &lhs, const table_reference &rhs);
    bool encode_table_metadata(uint8_t *buf, size_t buf_len, uint32_t magic, const table_metadata &metadata);
    format_status decode_table_metadata(const uint8_t *buf, size_t buf_len, uint32_t expected_magic,
                                        table_metadata *metadata_out);
    bool table_metadata_equal(const table_metadata &lhs, const table_metadata &rhs);
    bool encode_table_block_header(uint8_t *block, size_t block_len, const table_block_header &header);
    format_status decode_table_block_header(const uint8_t *block, size_t block_len, table_block_header *header_out);
    bool encode_table_index_header(uint8_t *block, size_t block_len, const table_index_header &header);
    format_status decode_table_index_header(const uint8_t *block, size_t block_len, table_index_header *header_out);
    bool encode_table_entry(uint8_t *buf, size_t buf_len, size_t offset, const table_entry &entry, size_t *size_out);
    format_status decode_table_entry(const uint8_t *buf, size_t buf_len, size_t offset, table_entry *entry_out);
    bool encode_table_index_entry(uint8_t *buf, size_t buf_len, size_t offset, const table_index_entry &entry, size_t *size_out);
    format_status decode_table_index_entry(const uint8_t *buf, size_t buf_len, size_t offset, table_index_entry *entry_out);

    bool value_ref_is_valid(const value_ref &reference, uint32_t bank_size);
    bool encode_value_chunk(uint8_t *chunk, size_t chunk_size, const value_chunk_header &header, const uint8_t *payload);
    format_status decode_value_chunk(const uint8_t *chunk, size_t chunk_size, value_chunk_header *header_out,
                                     const uint8_t **payload_out);
}
