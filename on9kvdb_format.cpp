#include <cstring>
#include <limits>

#include "on9kvdb_defs.hpp"

namespace
{
    static const constexpr uint32_t crc32_polynomial = 0xedb88320UL;

    constexpr uint32_t make_crc32_lut_entry(uint32_t value)
    {
        for (uint8_t bit = 0; bit < 8; bit += 1) {
            value = (value & 1U) ? ((value >> 1U) ^ crc32_polynomial) : (value >> 1U);
        }

        return value;
    }

    struct crc32_lut {
        uint32_t values[256] = {};

        constexpr crc32_lut()
        {
            for (size_t idx = 0; idx < 256; idx += 1) {
                values[idx] = make_crc32_lut_entry(static_cast<uint32_t>(idx));
            }
        }
    };

    static const constexpr crc32_lut crc32_table = {};

    constexpr uint32_t crc32_constexpr(const char *buf, size_t len)
    {
        uint32_t crc = UINT32_MAX;
        for (size_t idx = 0; idx < len; idx += 1) {
            const uint8_t table_idx = static_cast<uint8_t>(crc ^ static_cast<uint8_t>(buf[idx]));
            crc = crc32_table.values[table_idx] ^ (crc >> 8U);
        }

        return ~crc;
    }

    static_assert(crc32_constexpr("123456789", 9) == 0xcbf43926UL);

    bool is_range_valid(size_t buf_len, size_t offset, size_t len)
    {
        return offset <= buf_len && len <= buf_len - offset;
    }

    uint32_t calc_file_prefix_crc(const uint8_t *buf)
    {
        uint32_t crc = on9kvdb_def::calc_crc32_update(UINT32_MAX, buf, on9kvdb_def::file_prefix_checksum_offset);
        const uint8_t zeroes[sizeof(uint32_t)] = {};
        crc = on9kvdb_def::calc_crc32_update(crc, zeroes, sizeof(zeroes));
        return ~crc;
    }

    uint32_t calc_record_crc(const uint8_t *buf, size_t len, size_t checksum_offset)
    {
        uint32_t crc = on9kvdb_def::calc_crc32_update(UINT32_MAX, buf, checksum_offset);
        const uint8_t zeroes[sizeof(uint32_t)] = {};
        crc = on9kvdb_def::calc_crc32_update(crc, zeroes, sizeof(zeroes));
        const size_t suffix_offset = checksum_offset + sizeof(uint32_t);
        crc = on9kvdb_def::calc_crc32_update(crc, buf + suffix_offset, len - suffix_offset);
        return ~crc;
    }

    uint32_t magic_for_kind(on9kvdb_def::file_kind kind)
    {
        switch (kind) {
        case on9kvdb_def::file_kind::manifest:
            return on9kvdb_def::manifest_magic;
        case on9kvdb_def::file_kind::wal:
            return on9kvdb_def::wal_magic;
        case on9kvdb_def::file_kind::table:
            return on9kvdb_def::table_magic;
        case on9kvdb_def::file_kind::value_bank:
            return on9kvdb_def::value_bank_magic;
        default:
            return 0;
        }
    }

    bool is_zero_range(const uint8_t *buf, size_t offset, size_t len)
    {
        for (size_t idx = 0; idx < len; idx += 1) {
            if (buf[offset + idx] != 0) {
                return false;
            }
        }
        return true;
    }
}

bool on9kvdb_def::checked_add_size(size_t lhs, size_t rhs, size_t *result_out)
{
    if (result_out == nullptr || rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }

    *result_out = lhs + rhs;
    return true;
}

bool on9kvdb_def::checked_mul_size(size_t lhs, size_t rhs, size_t *result_out)
{
    if (result_out == nullptr || (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)) {
        return false;
    }

    *result_out = lhs * rhs;
    return true;
}

bool on9kvdb_def::checked_align_up_size(size_t value, size_t alignment, size_t *result_out)
{
    if (result_out == nullptr || alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        return false;
    }

    size_t adjusted = 0;
    if (!checked_add_size(value, alignment - 1U, &adjusted)) {
        return false;
    }

    *result_out = adjusted & ~(alignment - 1U);
    return true;
}

bool on9kvdb_def::checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out)
{
    if (result_out == nullptr || rhs > UINT64_MAX - lhs) {
        return false;
    }

    *result_out = lhs + rhs;
    return true;
}

bool on9kvdb_def::checked_mul_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out)
{
    if (result_out == nullptr || (lhs != 0 && rhs > UINT64_MAX / lhs)) {
        return false;
    }

    *result_out = lhs * rhs;
    return true;
}

bool on9kvdb_def::write_u16_le(uint8_t *buf, size_t buf_len, size_t offset, uint16_t value)
{
    if (buf == nullptr || !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    buf[offset] = static_cast<uint8_t>(value);
    buf[offset + 1] = static_cast<uint8_t>(value >> 8U);
    return true;
}

bool on9kvdb_def::write_u32_le(uint8_t *buf, size_t buf_len, size_t offset, uint32_t value)
{
    if (buf == nullptr || !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        buf[offset + idx] = static_cast<uint8_t>(value >> (idx * 8U));
    }
    return true;
}

bool on9kvdb_def::write_u64_le(uint8_t *buf, size_t buf_len, size_t offset, uint64_t value)
{
    if (buf == nullptr || !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        buf[offset + idx] = static_cast<uint8_t>(value >> (idx * 8U));
    }
    return true;
}

bool on9kvdb_def::read_u16_le(const uint8_t *buf, size_t buf_len, size_t offset, uint16_t *value_out)
{
    if (buf == nullptr || value_out == nullptr || !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    *value_out = static_cast<uint16_t>(buf[offset]) | static_cast<uint16_t>(static_cast<uint16_t>(buf[offset + 1]) << 8U);
    return true;
}

bool on9kvdb_def::read_u32_le(const uint8_t *buf, size_t buf_len, size_t offset, uint32_t *value_out)
{
    if (buf == nullptr || value_out == nullptr || !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    uint32_t value = 0;
    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        value |= static_cast<uint32_t>(buf[offset + idx]) << (idx * 8U);
    }
    *value_out = value;
    return true;
}

bool on9kvdb_def::read_u64_le(const uint8_t *buf, size_t buf_len, size_t offset, uint64_t *value_out)
{
    if (buf == nullptr || value_out == nullptr || !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    uint64_t value = 0;
    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        value |= static_cast<uint64_t>(buf[offset + idx]) << (idx * 8U);
    }
    *value_out = value;
    return true;
}

uint32_t on9kvdb_def::calc_crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (buf == nullptr && len != 0) {
        return crc;
    }

    for (size_t idx = 0; idx < len; idx += 1) {
        const uint8_t table_idx = static_cast<uint8_t>(crc ^ buf[idx]);
        crc = crc32_table.values[table_idx] ^ (crc >> 8U);
    }
    return crc;
}

uint32_t on9kvdb_def::calc_crc32(const uint8_t *buf, size_t len)
{
    return ~calc_crc32_update(UINT32_MAX, buf, len);
}

bool on9kvdb_def::validate_bytes(const uint8_t *data, uint16_t size, uint16_t maximum_size)
{
    return data != nullptr && size > 0 && size <= maximum_size;
}

uint64_t on9kvdb_def::make_handle_value(uint16_t slot, uint64_t generation)
{
    if (slot >= handle_slot_capacity || generation == 0 || generation > max_handle_generation) {
        return 0;
    }

    return (generation << handle_slot_bits) | (static_cast<uint64_t>(slot) + 1U);
}

bool on9kvdb_def::decode_handle_value(uint64_t value, uint16_t *slot_out, uint64_t *generation_out)
{
    if (slot_out == nullptr || generation_out == nullptr) {
        return false;
    }

    const uint64_t slot_token = value & handle_slot_mask;
    const uint64_t generation = value >> handle_slot_bits;
    if (slot_token == 0 || slot_token > handle_slot_capacity || generation == 0) {
        return false;
    }

    *slot_out = static_cast<uint16_t>(slot_token - 1U);
    *generation_out = generation;
    return true;
}

bool on9kvdb_def::is_handle_value(uint64_t value, uint16_t slot, uint64_t generation)
{
    return value != 0 && value == make_handle_value(slot, generation);
}

bool on9kvdb_def::encode_file_prefix(uint8_t *buf, size_t buf_len, uint32_t magic, file_kind kind, uint16_t flags,
                                     uint64_t generation, uint32_t payload_size)
{
    if (buf == nullptr || buf_len < file_prefix_size || magic == 0 || kind == file_kind::invalid || generation == 0) {
        return false;
    }

    memset(buf, 0, file_prefix_size);
    const bool encoded = write_u32_le(buf, buf_len, 0, magic) && write_u16_le(buf, buf_len, 4, storage_revision) &&
                         write_u16_le(buf, buf_len, 6, static_cast<uint16_t>(file_prefix_size)) &&
                         write_u16_le(buf, buf_len, 8, static_cast<uint16_t>(kind)) && write_u16_le(buf, buf_len, 10, flags) &&
                         write_u64_le(buf, buf_len, 12, generation) && write_u32_le(buf, buf_len, 20, payload_size);
    if (!encoded) {
        return false;
    }

    const uint32_t checksum = calc_file_prefix_crc(buf);
    return write_u32_le(buf, buf_len, file_prefix_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_file_prefix(const uint8_t *buf, size_t buf_len, uint32_t expected_magic,
                                                           file_kind expected_kind, decoded_file_prefix *prefix_out)
{
    if (buf == nullptr || prefix_out == nullptr || expected_magic == 0 || expected_kind == file_kind::invalid) {
        return format_status::invalid_argument;
    }
    if (buf_len < file_prefix_size) {
        return format_status::invalid_size;
    }

    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t size = 0;
    uint16_t kind_raw = 0;
    uint16_t flags = 0;
    uint64_t generation = 0;
    uint32_t payload_size = 0;
    uint32_t checksum = 0;
    const bool decoded = read_u32_le(buf, buf_len, 0, &magic) && read_u16_le(buf, buf_len, 4, &revision) &&
                         read_u16_le(buf, buf_len, 6, &size) && read_u16_le(buf, buf_len, 8, &kind_raw) &&
                         read_u16_le(buf, buf_len, 10, &flags) && read_u64_le(buf, buf_len, 12, &generation) &&
                         read_u32_le(buf, buf_len, 20, &payload_size) &&
                         read_u32_le(buf, buf_len, file_prefix_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }

    if (checksum != calc_file_prefix_crc(buf)) {
        return format_status::corrupt;
    }
    if (magic != expected_magic) {
        return format_status::invalid_magic;
    }
    if (revision > storage_revision) {
        return format_status::new_version;
    }
    if (revision != storage_revision) {
        return format_status::invalid_revision;
    }
    if (size != file_prefix_size) {
        return format_status::invalid_size;
    }
    if (kind_raw != static_cast<uint16_t>(expected_kind)) {
        return format_status::invalid_kind;
    }
    if (generation == 0) {
        return format_status::invalid_generation;
    }

    decoded_file_prefix prefix = {};
    prefix.magic = magic;
    prefix.revision = revision;
    prefix.kind = expected_kind;
    prefix.flags = flags;
    prefix.generation = generation;
    prefix.payload_size = payload_size;
    prefix.checksum = checksum;
    *prefix_out = prefix;
    return format_status::ok;
}

bool on9kvdb_def::validate_storage_geometry(const storage_geometry &geometry)
{
    if (geometry.provisioned_size == 0 || geometry.max_live_bytes == 0 || geometry.max_live_bytes > geometry.provisioned_size ||
        geometry.manifest_size != manifest_file_size || geometry.wal_size < wal_record_region_offset + wal_frame_size ||
        geometry.wal_count != wal_file_count || geometry.table_size <= identity_region_size || geometry.table_count < 4 ||
        geometry.table_count > max_table_count || (geometry.table_count & 1U) != 0 ||
        geometry.value_bank_size <= identity_region_size || geometry.value_bank_count != value_bank_count ||
        geometry.alignment != format_alignment || geometry.manifest_size % format_alignment != 0 ||
        geometry.wal_size % format_alignment != 0 || geometry.table_size % format_alignment != 0 ||
        geometry.value_bank_size % format_alignment != 0) {
        return false;
    }

    uint64_t wal_bytes = 0;
    uint64_t table_bytes = 0;
    uint64_t value_bank_bytes = 0;
    uint64_t table_payload_bytes = 0;
    uint64_t calculated_size = geometry.manifest_size;
    return checked_mul_u64(geometry.wal_size, geometry.wal_count, &wal_bytes) &&
           checked_mul_u64(geometry.table_size, geometry.table_count, &table_bytes) &&
           checked_mul_u64(geometry.value_bank_size, geometry.value_bank_count, &value_bank_bytes) &&
           checked_mul_u64(geometry.table_size - identity_region_size, geometry.table_count, &table_payload_bytes) &&
           checked_add_u64(calculated_size, wal_bytes, &calculated_size) &&
           checked_add_u64(calculated_size, table_bytes, &calculated_size) &&
           checked_add_u64(calculated_size, value_bank_bytes, &calculated_size) && calculated_size == geometry.provisioned_size &&
           geometry.max_live_bytes <= table_payload_bytes;
}

bool on9kvdb_def::storage_geometry_equal(const storage_geometry &lhs, const storage_geometry &rhs)
{
    return lhs.provisioned_size == rhs.provisioned_size && lhs.max_live_bytes == rhs.max_live_bytes &&
           lhs.manifest_size == rhs.manifest_size && lhs.wal_size == rhs.wal_size && lhs.wal_count == rhs.wal_count &&
           lhs.table_size == rhs.table_size && lhs.table_count == rhs.table_count && lhs.value_bank_size == rhs.value_bank_size &&
           lhs.value_bank_count == rhs.value_bank_count && lhs.alignment == rhs.alignment;
}

bool on9kvdb_def::validate_logical_limits(const logical_limits &limits)
{
    const uint32_t maximum_entry_size = table_entry_header_size + 2U * max_name_len + inline_value_len;
    return limits.wal_frame_bytes == wal_frame_size && limits.max_namespaces > 0 && limits.max_namespaces <= UINT16_MAX &&
           limits.max_open_handles > 0 && limits.max_open_handles <= UINT16_MAX && limits.memtable_entries >= 16 &&
           (limits.memtable_entries & (limits.memtable_entries - 1U)) == 0 && limits.memtable_data_bytes >= inline_value_len &&
           limits.max_transaction_mutations > 0 && limits.max_transaction_mutations <= max_transaction_mutations &&
           limits.transaction_staging_bytes >= inline_value_len && limits.inline_value_bytes == inline_value_len &&
           limits.sstable_block_bytes % format_alignment == 0 &&
           limits.sstable_block_bytes > table_block_header_size + maximum_entry_size;
}

bool on9kvdb_def::validate_compaction_capacity(const storage_geometry &geometry, const logical_limits &limits)
{
    if (!validate_storage_geometry(geometry) || !validate_logical_limits(limits) ||
        geometry.table_size <= table_data_region_offset + limits.sstable_block_bytes + table_footer_slot_size) {
        return false;
    }

    const uint64_t data_region_bytes =
        geometry.table_size - table_data_region_offset - limits.sstable_block_bytes - table_footer_slot_size;
    if (data_region_bytes % limits.sstable_block_bytes != 0) {
        return false;
    }

    const uint64_t data_blocks_per_table = data_region_bytes / limits.sstable_block_bytes;
    uint64_t bank_block_count = 0;
    uint64_t maximum_index_bytes = 0;
    const uint64_t maximum_index_entry_size = (table_index_entry_header_size + 2U * max_name_len + 3U) & ~UINT64_C(3);
    if (!checked_mul_u64(data_blocks_per_table, geometry.table_count / 2U, &bank_block_count) ||
        !checked_mul_u64(data_blocks_per_table, maximum_index_entry_size, &maximum_index_bytes) ||
        maximum_index_bytes > limits.sstable_block_bytes - table_index_header_size) {
        return false;
    }
    const uint64_t block_payload_bytes = limits.sstable_block_bytes - table_block_header_size;

    // Chunk allocation has its highest physical/logical ratio at the smallest
    // external value.  Reserve enough bank space for the conservative case in
    // which the complete logical-state budget consists of such records.  This
    // prevents accepting a geometry that can fill SSTables with a live set
    // which can never be copied into the other value bank.
    const uint64_t minimum_external_value = static_cast<uint64_t>(limits.inline_value_bytes) + 1U;
    const uint64_t minimum_external_record = (table_entry_header_size + 2U + minimum_external_value + 7U) & ~UINT64_C(7);
    const uint64_t chunks_per_minimum_value = (minimum_external_value + value_chunk_payload_size - 1U) / value_chunk_payload_size;
    uint64_t record_count_ceiling = 0;
    uint64_t physical_bytes_per_record = 0;
    uint64_t required_value_bytes = 0;
    uint64_t rounded_live_bytes = 0;
    if (!checked_add_u64(geometry.max_live_bytes, minimum_external_record - 1U, &rounded_live_bytes) ||
        !checked_mul_u64(chunks_per_minimum_value, value_chunk_size, &physical_bytes_per_record)) {
        return false;
    }
    record_count_ceiling = rounded_live_bytes / minimum_external_record;
    if (!checked_mul_u64(record_count_ceiling, physical_bytes_per_record, &required_value_bytes) ||
        !checked_add_u64(required_value_bytes, identity_region_size, &required_value_bytes) ||
        required_value_bytes > geometry.value_bank_size) {
        return false;
    }

    // Next-fit block packing guarantees that every completed pair of blocks contains more than one block of encoded records.
    // Reserving the unpaired block gives a simple hard bound for arbitrary permitted record-size mixtures.
    const uint64_t guaranteed_logical_bytes = (bank_block_count / 2U) * block_payload_bytes;
    return geometry.max_live_bytes <= guaranteed_logical_bytes;
}

bool on9kvdb_def::logical_limits_equal(const logical_limits &lhs, const logical_limits &rhs)
{
    return lhs.wal_frame_bytes == rhs.wal_frame_bytes && lhs.max_namespaces == rhs.max_namespaces &&
           lhs.max_open_handles == rhs.max_open_handles && lhs.memtable_entries == rhs.memtable_entries &&
           lhs.memtable_data_bytes == rhs.memtable_data_bytes && lhs.max_transaction_mutations == rhs.max_transaction_mutations &&
           lhs.transaction_staging_bytes == rhs.transaction_staging_bytes && lhs.sstable_block_bytes == rhs.sstable_block_bytes &&
           lhs.inline_value_bytes == rhs.inline_value_bytes;
}

bool on9kvdb_def::composite_key_equal(const composite_key &lhs, const composite_key &rhs)
{
    return lhs.namespace_size == rhs.namespace_size && lhs.key_size == rhs.key_size &&
           memcmp(lhs.namespace_name, rhs.namespace_name, lhs.namespace_size) == 0 && memcmp(lhs.key, rhs.key, lhs.key_size) == 0;
}

int on9kvdb_def::compare_composite_key(const composite_key &lhs, const composite_key &rhs)
{
    const size_t namespace_common = lhs.namespace_size < rhs.namespace_size ? lhs.namespace_size : rhs.namespace_size;
    const int namespace_comparison = memcmp(lhs.namespace_name, rhs.namespace_name, namespace_common);
    if (namespace_comparison != 0) {
        return namespace_comparison;
    }
    if (lhs.namespace_size != rhs.namespace_size) {
        return lhs.namespace_size < rhs.namespace_size ? -1 : 1;
    }

    const size_t key_common = lhs.key_size < rhs.key_size ? lhs.key_size : rhs.key_size;
    const int key_comparison = memcmp(lhs.key, rhs.key, key_common);
    if (key_comparison != 0) {
        return key_comparison;
    }
    if (lhs.key_size != rhs.key_size) {
        return lhs.key_size < rhs.key_size ? -1 : 1;
    }
    return 0;
}

bool on9kvdb_def::table_reference_equal(const table_reference &lhs, const table_reference &rhs)
{
    if (lhs.active != rhs.active) {
        return false;
    }
    if (!lhs.active) {
        return true;
    }
    return lhs.level == rhs.level && lhs.slot == rhs.slot && lhs.data_block_count == rhs.data_block_count &&
           lhs.generation == rhs.generation && lhs.min_sequence == rhs.min_sequence && lhs.max_sequence == rhs.max_sequence &&
           lhs.entry_count == rhs.entry_count && lhs.data_bytes == rhs.data_bytes && lhs.content_checksum == rhs.content_checksum;
}

namespace
{
    bool valid_composite_key(const on9kvdb_def::composite_key &key)
    {
        return key.namespace_size > 0 && key.namespace_size <= on9kvdb_def::max_name_len && key.key_size > 0 &&
               key.key_size <= on9kvdb_def::max_name_len && on9kvdb_def::validate_bytes(key.namespace_name, key.namespace_size) &&
               on9kvdb_def::validate_bytes(key.key, key.key_size);
    }

    bool valid_table_reference(const on9kvdb_def::table_reference &reference, uint32_t expected_slot,
                               uint64_t safe_checkpoint_sequence)
    {
        return reference.active && reference.level == 0 && reference.slot == expected_slot && reference.generation > 0 &&
               reference.min_sequence > 0 && reference.max_sequence >= reference.min_sequence &&
               reference.max_sequence <= safe_checkpoint_sequence && reference.entry_count > 0 &&
               reference.data_block_count > 0 && reference.data_block_count <= reference.entry_count && reference.data_bytes > 0;
    }

    bool encode_manifest_table_reference(uint8_t *buf, size_t buf_len, size_t offset,
                                         const on9kvdb_def::table_reference &reference)
    {
        if (!reference.active) {
            return true;
        }
        if (!on9kvdb_def::write_u32_le(buf, buf_len, offset + 8, reference.slot) ||
            !on9kvdb_def::write_u32_le(buf, buf_len, offset + 12, reference.data_block_count) ||
            !on9kvdb_def::write_u64_le(buf, buf_len, offset + 16, reference.generation) ||
            !on9kvdb_def::write_u64_le(buf, buf_len, offset + 24, reference.min_sequence) ||
            !on9kvdb_def::write_u64_le(buf, buf_len, offset + 32, reference.max_sequence) ||
            !on9kvdb_def::write_u32_le(buf, buf_len, offset + 40, reference.entry_count) ||
            !on9kvdb_def::write_u32_le(buf, buf_len, offset + 44, reference.data_bytes) ||
            !on9kvdb_def::write_u32_le(buf, buf_len, offset + 48, reference.content_checksum)) {
            return false;
        }
        buf[offset] = on9kvdb_def::table_reference_flag_active;
        buf[offset + 1] = reference.level;
        return true;
    }

    on9kvdb_def::format_status decode_manifest_table_reference(const uint8_t *buf, size_t buf_len, size_t offset,
                                                               on9kvdb_def::table_reference *reference_out)
    {
        if (!is_range_valid(buf_len, offset, on9kvdb_def::manifest_table_reference_size) || reference_out == nullptr) {
            return on9kvdb_def::format_status::invalid_size;
        }
        if (is_zero_range(buf, offset, on9kvdb_def::manifest_table_reference_size)) {
            *reference_out = {};
            return on9kvdb_def::format_status::ok;
        }
        if (buf[offset] != on9kvdb_def::table_reference_flag_active || buf[offset + 1] != 0 ||
            (buf[offset] & ~on9kvdb_def::table_reference_flag_active) != 0 || !is_zero_range(buf, offset + 2, 6) ||
            !is_zero_range(buf, offset + 52, 4)) {
            return on9kvdb_def::format_status::corrupt;
        }

        on9kvdb_def::table_reference reference = {};
        reference.active = true;
        reference.level = buf[offset + 1];
        if (!on9kvdb_def::read_u32_le(buf, buf_len, offset + 8, &reference.slot) ||
            !on9kvdb_def::read_u32_le(buf, buf_len, offset + 12, &reference.data_block_count) ||
            !on9kvdb_def::read_u64_le(buf, buf_len, offset + 16, &reference.generation) ||
            !on9kvdb_def::read_u64_le(buf, buf_len, offset + 24, &reference.min_sequence) ||
            !on9kvdb_def::read_u64_le(buf, buf_len, offset + 32, &reference.max_sequence) ||
            !on9kvdb_def::read_u32_le(buf, buf_len, offset + 40, &reference.entry_count) ||
            !on9kvdb_def::read_u32_le(buf, buf_len, offset + 44, &reference.data_bytes) ||
            !on9kvdb_def::read_u32_le(buf, buf_len, offset + 48, &reference.content_checksum)) {
            return on9kvdb_def::format_status::invalid_size;
        }
        *reference_out = reference;
        return on9kvdb_def::format_status::ok;
    }
}

bool on9kvdb_def::encode_manifest_record(uint8_t *buf, size_t buf_len, const manifest_record &record)
{
    uint32_t active_table_count = 0;
    uint64_t greatest_table_generation = 0;
    uint64_t greatest_table_sequence = 0;
    const bool bank_geometry_valid = record.geometry.table_count >= 4 && record.geometry.table_count <= max_table_count &&
                                     (record.geometry.table_count & 1U) == 0 && record.active_table_bank < 2;
    const uint32_t bank_size = bank_geometry_valid ? record.geometry.table_count / 2U : 0;
    const uint32_t bank_start = record.active_table_bank * bank_size;
    const uint32_t bank_end = bank_start + bank_size;
    bool table_references_valid = bank_geometry_valid;
    for (uint32_t slot = 0; table_references_valid && slot < max_table_count; slot += 1) {
        const table_reference &reference = record.tables[slot];
        if (!reference.active) {
            continue;
        }
        if (slot < bank_start || slot >= bank_end || !valid_table_reference(reference, slot, record.safe_checkpoint_sequence)) {
            table_references_valid = false;
            break;
        }
        for (uint32_t previous = bank_start; previous < slot; previous += 1) {
            if (record.tables[previous].active && record.tables[previous].generation == reference.generation) {
                table_references_valid = false;
                break;
            }
        }
        if (!table_references_valid) {
            break;
        }
        active_table_count += 1;
        if (reference.generation > greatest_table_generation) {
            greatest_table_generation = reference.generation;
        }
        if (reference.max_sequence > greatest_table_sequence) {
            greatest_table_sequence = reference.max_sequence;
        }
    }

    if (buf == nullptr || buf_len < manifest_record_size || record.generation == 0 || record.database_id == 0 ||
        (record.state != manifest_state_provisioning_owned && record.state != manifest_state_ready) ||
        (record.state == manifest_state_ready && record.generation < 3) ||
        !validate_compaction_capacity(record.geometry, record.limits) || record.active_wal_slot >= wal_file_count ||
        record.next_table_generation == 0 || !table_references_valid ||
        greatest_table_generation >= record.next_table_generation ||
        (active_table_count != 0 && greatest_table_sequence != record.safe_checkpoint_sequence) ||
        (record.state == manifest_state_provisioning_owned &&
         (record.wal_generation[0] != 0 || record.wal_generation[1] != 0 || record.safe_checkpoint_sequence != 0 ||
          active_table_count != 0 || record.next_table_generation != 1 || record.active_table_bank != 0 ||
          record.active_value_bank != 0 || record.value_bank_generation[0] != 1 || record.value_bank_generation[1] != 1 ||
          record.value_bank_tail[0] != identity_region_size || record.value_bank_tail[1] != identity_region_size)) ||
        (record.state == manifest_state_ready &&
         (record.wal_generation[record.active_wal_slot] == 0 || record.active_value_bank >= value_bank_count ||
          record.value_bank_generation[0] == 0 || record.value_bank_generation[1] == 0 ||
          record.value_bank_tail[0] < identity_region_size || record.value_bank_tail[1] < identity_region_size ||
          record.value_bank_tail[0] > record.geometry.value_bank_size ||
          record.value_bank_tail[1] > record.geometry.value_bank_size || record.value_bank_tail[0] % value_chunk_size != 0 ||
          record.value_bank_tail[1] % value_chunk_size != 0))) {
        return false;
    }

    memset(buf, 0, manifest_record_size);
    if (!encode_file_prefix(buf, buf_len, manifest_magic, file_kind::manifest, 0, record.generation,
                            static_cast<uint32_t>(manifest_record_size - file_prefix_size)) ||
        !write_u64_le(buf, buf_len, 28, record.database_id) || !write_u16_le(buf, buf_len, 36, record.state) ||
        !write_u16_le(buf, buf_len, 38, geometry_revision) || !write_u32_le(buf, buf_len, 40, record.geometry.alignment) ||
        !write_u32_le(buf, buf_len, 44, record.geometry.manifest_size) ||
        !write_u32_le(buf, buf_len, 48, record.geometry.wal_size) || !write_u32_le(buf, buf_len, 52, record.geometry.wal_count) ||
        !write_u32_le(buf, buf_len, 56, record.geometry.table_size) ||
        !write_u32_le(buf, buf_len, 60, record.geometry.table_count) ||
        !write_u32_le(buf, buf_len, 64, record.geometry.value_bank_size) ||
        !write_u32_le(buf, buf_len, 68, record.geometry.value_bank_count) ||
        !write_u64_le(buf, buf_len, 72, record.geometry.max_live_bytes) ||
        !write_u64_le(buf, buf_len, 80, record.geometry.provisioned_size) ||
        !write_u16_le(buf, buf_len, 88, logical_limits_revision) ||
        !write_u32_le(buf, buf_len, 92, record.limits.wal_frame_bytes) ||
        !write_u32_le(buf, buf_len, 96, record.limits.max_namespaces) ||
        !write_u32_le(buf, buf_len, 100, record.limits.max_open_handles) ||
        !write_u32_le(buf, buf_len, 104, record.limits.memtable_entries) ||
        !write_u32_le(buf, buf_len, 108, record.limits.memtable_data_bytes) ||
        !write_u32_le(buf, buf_len, 112, record.limits.max_transaction_mutations) ||
        !write_u32_le(buf, buf_len, 116, record.limits.transaction_staging_bytes) ||
        !write_u32_le(buf, buf_len, 120, record.limits.sstable_block_bytes) ||
        !write_u32_le(buf, buf_len, 124, record.limits.inline_value_bytes) ||
        !write_u32_le(buf, buf_len, 128, record.active_wal_slot) || !write_u64_le(buf, buf_len, 132, record.wal_generation[0]) ||
        !write_u64_le(buf, buf_len, 140, record.wal_generation[1]) ||
        !write_u64_le(buf, buf_len, 148, record.safe_checkpoint_sequence) ||
        !write_u64_le(buf, buf_len, 156, record.next_table_generation) || !write_u32_le(buf, buf_len, 164, active_table_count) ||
        !write_u32_le(buf, buf_len, 168, record.active_table_bank) ||
        !write_u32_le(buf, buf_len, 172, record.active_value_bank) ||
        !write_u64_le(buf, buf_len, 176, record.value_bank_generation[0]) ||
        !write_u64_le(buf, buf_len, 184, record.value_bank_generation[1]) ||
        !write_u32_le(buf, buf_len, 192, record.value_bank_tail[0]) ||
        !write_u32_le(buf, buf_len, 196, record.value_bank_tail[1])) {
        return false;
    }
    for (uint32_t slot = 0; slot < max_table_count; slot += 1) {
        const size_t offset = manifest_table_reference_offset + static_cast<size_t>(slot) * manifest_table_reference_size;
        if (!encode_manifest_table_reference(buf, buf_len, offset, record.tables[slot])) {
            return false;
        }
    }

    const uint32_t checksum = calc_record_crc(buf, manifest_record_size, manifest_record_checksum_offset);
    return write_u32_le(buf, buf_len, manifest_record_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_manifest_record(const uint8_t *buf, size_t buf_len, manifest_record *record_out)
{
    if (buf == nullptr || record_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (buf_len < manifest_record_size) {
        return format_status::invalid_size;
    }

    decoded_file_prefix prefix = {};
    const format_status prefix_status = decode_file_prefix(buf, buf_len, manifest_magic, file_kind::manifest, &prefix);
    if (prefix_status != format_status::ok) {
        return prefix_status;
    }
    if (prefix.payload_size != manifest_record_size - file_prefix_size) {
        return format_status::invalid_size;
    }

    manifest_record record = {};
    uint16_t geometry_format = 0;
    uint16_t limits_format = 0;
    uint32_t encoded_active_table_count = 0;
    uint32_t checksum = 0;
    const bool decoded =
        read_u64_le(buf, buf_len, 28, &record.database_id) && read_u16_le(buf, buf_len, 36, &record.state) &&
        read_u16_le(buf, buf_len, 38, &geometry_format) && read_u32_le(buf, buf_len, 40, &record.geometry.alignment) &&
        read_u32_le(buf, buf_len, 44, &record.geometry.manifest_size) &&
        read_u32_le(buf, buf_len, 48, &record.geometry.wal_size) && read_u32_le(buf, buf_len, 52, &record.geometry.wal_count) &&
        read_u32_le(buf, buf_len, 56, &record.geometry.table_size) &&
        read_u32_le(buf, buf_len, 60, &record.geometry.table_count) &&
        read_u32_le(buf, buf_len, 64, &record.geometry.value_bank_size) &&
        read_u32_le(buf, buf_len, 68, &record.geometry.value_bank_count) &&
        read_u64_le(buf, buf_len, 72, &record.geometry.max_live_bytes) &&
        read_u64_le(buf, buf_len, 80, &record.geometry.provisioned_size) && read_u16_le(buf, buf_len, 88, &limits_format) &&
        read_u32_le(buf, buf_len, 92, &record.limits.wal_frame_bytes) &&
        read_u32_le(buf, buf_len, 96, &record.limits.max_namespaces) &&
        read_u32_le(buf, buf_len, 100, &record.limits.max_open_handles) &&
        read_u32_le(buf, buf_len, 104, &record.limits.memtable_entries) &&
        read_u32_le(buf, buf_len, 108, &record.limits.memtable_data_bytes) &&
        read_u32_le(buf, buf_len, 112, &record.limits.max_transaction_mutations) &&
        read_u32_le(buf, buf_len, 116, &record.limits.transaction_staging_bytes) &&
        read_u32_le(buf, buf_len, 120, &record.limits.sstable_block_bytes) &&
        read_u32_le(buf, buf_len, 124, &record.limits.inline_value_bytes) &&
        read_u32_le(buf, buf_len, 128, &record.active_wal_slot) && read_u64_le(buf, buf_len, 132, &record.wal_generation[0]) &&
        read_u64_le(buf, buf_len, 140, &record.wal_generation[1]) &&
        read_u64_le(buf, buf_len, 148, &record.safe_checkpoint_sequence) &&
        read_u64_le(buf, buf_len, 156, &record.next_table_generation) &&
        read_u32_le(buf, buf_len, 164, &encoded_active_table_count) &&
        read_u32_le(buf, buf_len, 168, &record.active_table_bank) && read_u32_le(buf, buf_len, 172, &record.active_value_bank) &&
        read_u64_le(buf, buf_len, 176, &record.value_bank_generation[0]) &&
        read_u64_le(buf, buf_len, 184, &record.value_bank_generation[1]) &&
        read_u32_le(buf, buf_len, 192, &record.value_bank_tail[0]) &&
        read_u32_le(buf, buf_len, 196, &record.value_bank_tail[1]) &&
        read_u32_le(buf, buf_len, manifest_record_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }

    uint32_t active_table_count = 0;
    uint64_t greatest_table_generation = 0;
    uint64_t greatest_table_sequence = 0;
    const bool bank_geometry_valid = record.geometry.table_count >= 4 && record.geometry.table_count <= max_table_count &&
                                     (record.geometry.table_count & 1U) == 0 && record.active_table_bank < 2;
    const uint32_t bank_size = bank_geometry_valid ? record.geometry.table_count / 2U : 0;
    const uint32_t bank_start = record.active_table_bank * bank_size;
    const uint32_t bank_end = bank_start + bank_size;
    for (uint32_t slot = 0; slot < max_table_count; slot += 1) {
        const size_t offset = manifest_table_reference_offset + static_cast<size_t>(slot) * manifest_table_reference_size;
        const format_status status = decode_manifest_table_reference(buf, buf_len, offset, &record.tables[slot]);
        if (status != format_status::ok) {
            return status;
        }
        if (!record.tables[slot].active) {
            continue;
        }
        if (!bank_geometry_valid || slot < bank_start || slot >= bank_end ||
            !valid_table_reference(record.tables[slot], slot, record.safe_checkpoint_sequence)) {
            return format_status::corrupt;
        }
        for (uint32_t previous = bank_start; previous < slot; previous += 1) {
            if (record.tables[previous].active && record.tables[previous].generation == record.tables[slot].generation) {
                return format_status::corrupt;
            }
        }
        active_table_count += 1;
        if (record.tables[slot].generation > greatest_table_generation) {
            greatest_table_generation = record.tables[slot].generation;
        }
        if (record.tables[slot].max_sequence > greatest_table_sequence) {
            greatest_table_sequence = record.tables[slot].max_sequence;
        }
    }

    record.generation = prefix.generation;
    if (record.database_id == 0 || (record.state != manifest_state_provisioning_owned && record.state != manifest_state_ready) ||
        (record.state == manifest_state_ready && record.generation < 3) || geometry_format != geometry_revision ||
        limits_format != logical_limits_revision || !is_zero_range(buf, 90, 2) || !is_zero_range(buf, 200, 24) ||
        !is_zero_range(buf, manifest_table_reference_offset + max_table_count * manifest_table_reference_size,
                       manifest_record_checksum_offset -
                           (manifest_table_reference_offset + max_table_count * manifest_table_reference_size)) ||
        !validate_compaction_capacity(record.geometry, record.limits) || record.active_wal_slot >= wal_file_count ||
        record.next_table_generation == 0 || active_table_count != encoded_active_table_count ||
        greatest_table_generation >= record.next_table_generation ||
        (active_table_count != 0 && greatest_table_sequence != record.safe_checkpoint_sequence) ||
        (record.state == manifest_state_provisioning_owned &&
         (record.wal_generation[0] != 0 || record.wal_generation[1] != 0 || record.safe_checkpoint_sequence != 0 ||
          active_table_count != 0 || record.next_table_generation != 1 || record.active_table_bank != 0 ||
          record.active_value_bank != 0 || record.value_bank_generation[0] != 1 || record.value_bank_generation[1] != 1 ||
          record.value_bank_tail[0] != identity_region_size || record.value_bank_tail[1] != identity_region_size)) ||
        (record.state == manifest_state_ready &&
         (record.wal_generation[record.active_wal_slot] == 0 || record.active_value_bank >= value_bank_count ||
          record.value_bank_generation[0] == 0 || record.value_bank_generation[1] == 0 ||
          record.value_bank_tail[0] < identity_region_size || record.value_bank_tail[1] < identity_region_size ||
          record.value_bank_tail[0] > record.geometry.value_bank_size ||
          record.value_bank_tail[1] > record.geometry.value_bank_size || record.value_bank_tail[0] % value_chunk_size != 0 ||
          record.value_bank_tail[1] % value_chunk_size != 0))) {
        return format_status::corrupt;
    }
    if (checksum != calc_record_crc(buf, manifest_record_size, manifest_record_checksum_offset)) {
        return format_status::corrupt;
    }

    *record_out = record;
    return format_status::ok;
}

bool on9kvdb_def::encode_file_identity(uint8_t *buf, size_t buf_len, const file_identity &identity)
{
    const uint32_t magic = magic_for_kind(identity.kind);
    if (buf == nullptr || buf_len < file_identity_size || magic == 0 || identity.kind == file_kind::manifest ||
        identity.generation == 0 || identity.database_id == 0 || identity.file_size <= identity_region_size ||
        identity.file_size > max_storage_object_size || identity.file_size % format_alignment != 0) {
        return false;
    }

    memset(buf, 0, file_identity_size);
    if (!encode_file_prefix(buf, buf_len, magic, identity.kind, file_prefix_flag_identity, identity.generation,
                            static_cast<uint32_t>(file_identity_size - file_prefix_size)) ||
        !write_u64_le(buf, buf_len, 28, identity.database_id) || !write_u64_le(buf, buf_len, 36, identity.file_size) ||
        !write_u32_le(buf, buf_len, 44, identity.slot)) {
        return false;
    }

    const uint32_t checksum = calc_record_crc(buf, file_identity_size, file_identity_checksum_offset);
    return write_u32_le(buf, buf_len, file_identity_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_file_identity(const uint8_t *buf, size_t buf_len, file_kind expected_kind,
                                                             file_identity *identity_out)
{
    const uint32_t magic = magic_for_kind(expected_kind);
    if (buf == nullptr || identity_out == nullptr || magic == 0 || expected_kind == file_kind::manifest) {
        return format_status::invalid_argument;
    }
    if (buf_len < file_identity_size) {
        return format_status::invalid_size;
    }

    decoded_file_prefix prefix = {};
    const format_status prefix_status = decode_file_prefix(buf, buf_len, magic, expected_kind, &prefix);
    if (prefix_status != format_status::ok) {
        return prefix_status;
    }
    if (prefix.flags != file_prefix_flag_identity || prefix.payload_size != file_identity_size - file_prefix_size) {
        return format_status::corrupt;
    }

    file_identity identity = {};
    uint32_t checksum = 0;
    const bool decoded = read_u64_le(buf, buf_len, 28, &identity.database_id) &&
                         read_u64_le(buf, buf_len, 36, &identity.file_size) && read_u32_le(buf, buf_len, 44, &identity.slot) &&
                         read_u32_le(buf, buf_len, file_identity_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }

    identity.generation = prefix.generation;
    identity.kind = expected_kind;
    if (identity.database_id == 0 || identity.file_size <= identity_region_size || identity.file_size > max_storage_object_size ||
        identity.file_size % format_alignment != 0 || !is_zero_range(buf, 48, 4)) {
        return format_status::corrupt;
    }
    if (checksum != calc_record_crc(buf, file_identity_size, file_identity_checksum_offset)) {
        return format_status::corrupt;
    }

    *identity_out = identity;
    return format_status::ok;
}

bool on9kvdb_def::encode_wal_header(uint8_t *buf, size_t buf_len, const wal_header &header)
{
    if (buf == nullptr || buf_len < wal_header_size || header.database_id == 0 || header.generation == 0 ||
        header.first_transaction_sequence == 0 || header.slot >= wal_file_count || header.state != wal_header_state_active ||
        header.record_region_start != wal_record_region_offset || header.record_region_end <= header.record_region_start ||
        header.record_region_end % wal_frame_size != 0 || header.frame_size != wal_frame_size) {
        return false;
    }

    memset(buf, 0, wal_header_size);
    if (!write_u32_le(buf, buf_len, 0, wal_header_magic) || !write_u16_le(buf, buf_len, 4, wal_header_revision) ||
        !write_u16_le(buf, buf_len, 6, static_cast<uint16_t>(wal_header_size)) ||
        !write_u64_le(buf, buf_len, 8, header.database_id) || !write_u32_le(buf, buf_len, 16, header.slot) ||
        !write_u16_le(buf, buf_len, 20, header.state) || !write_u64_le(buf, buf_len, 24, header.generation) ||
        !write_u32_le(buf, buf_len, 32, header.record_region_start) ||
        !write_u32_le(buf, buf_len, 36, header.record_region_end) || !write_u32_le(buf, buf_len, 40, header.frame_size) ||
        !write_u64_le(buf, buf_len, 48, header.first_transaction_sequence)) {
        return false;
    }

    const uint32_t checksum = calc_record_crc(buf, wal_header_size, wal_header_checksum_offset);
    return write_u32_le(buf, buf_len, wal_header_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_wal_header(const uint8_t *buf, size_t buf_len, wal_header *header_out)
{
    if (buf == nullptr || header_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (buf_len < wal_header_size) {
        return format_status::invalid_size;
    }

    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t size = 0;
    uint16_t reserved16 = 0;
    uint32_t reserved32 = 0;
    uint32_t checksum = 0;
    wal_header header = {};
    const bool decoded =
        read_u32_le(buf, buf_len, 0, &magic) && read_u16_le(buf, buf_len, 4, &revision) && read_u16_le(buf, buf_len, 6, &size) &&
        read_u64_le(buf, buf_len, 8, &header.database_id) && read_u32_le(buf, buf_len, 16, &header.slot) &&
        read_u16_le(buf, buf_len, 20, &header.state) && read_u16_le(buf, buf_len, 22, &reserved16) &&
        read_u64_le(buf, buf_len, 24, &header.generation) && read_u32_le(buf, buf_len, 32, &header.record_region_start) &&
        read_u32_le(buf, buf_len, 36, &header.record_region_end) && read_u32_le(buf, buf_len, 40, &header.frame_size) &&
        read_u32_le(buf, buf_len, 44, &reserved32) && read_u64_le(buf, buf_len, 48, &header.first_transaction_sequence) &&
        read_u32_le(buf, buf_len, 56, &reserved32) && read_u32_le(buf, buf_len, wal_header_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (checksum != calc_record_crc(buf, wal_header_size, wal_header_checksum_offset)) {
        return format_status::corrupt;
    }
    if (magic != wal_header_magic) {
        return format_status::invalid_magic;
    }
    if (revision > wal_header_revision) {
        return format_status::new_version;
    }
    if (revision != wal_header_revision) {
        return format_status::invalid_revision;
    }
    if (size != wal_header_size) {
        return format_status::invalid_size;
    }
    if (header.database_id == 0 || header.generation == 0 || header.first_transaction_sequence == 0 ||
        header.slot >= wal_file_count || header.state != wal_header_state_active ||
        header.record_region_start != wal_record_region_offset || header.record_region_end <= header.record_region_start ||
        header.record_region_end % wal_frame_size != 0 || header.frame_size != wal_frame_size || reserved16 != 0 ||
        !is_zero_range(buf, 44, 4) || !is_zero_range(buf, 56, 4)) {
        return format_status::corrupt;
    }

    *header_out = header;
    return format_status::ok;
}

bool on9kvdb_def::wal_header_equal(const wal_header &lhs, const wal_header &rhs)
{
    return lhs.database_id == rhs.database_id && lhs.generation == rhs.generation &&
           lhs.first_transaction_sequence == rhs.first_transaction_sequence && lhs.slot == rhs.slot &&
           lhs.record_region_start == rhs.record_region_start && lhs.record_region_end == rhs.record_region_end &&
           lhs.frame_size == rhs.frame_size && lhs.state == rhs.state;
}

bool on9kvdb_def::encode_wal_frame(uint8_t *frame, size_t frame_len, const wal_frame_header &header, const uint8_t *payload,
                                   size_t payload_len)
{
    const bool final_frame = header.frame_count > 0 && header.frame_index + 1U == header.frame_count;
    if (frame == nullptr || frame_len < wal_frame_size || (payload == nullptr && payload_len != 0) ||
        payload_len != header.payload_size || payload_len > wal_frame_payload_capacity || header.database_id == 0 ||
        header.wal_generation == 0 || header.transaction_sequence == 0 || header.frame_count == 0 ||
        header.frame_index >= header.frame_count || header.mutation_count == 0 ||
        header.mutation_count > max_transaction_mutations || header.transaction_payload_size == 0 ||
        header.payload_size > header.transaction_payload_size || (header.flags & ~wal_frame_flag_commit) != 0 ||
        final_frame != ((header.flags & wal_frame_flag_commit) != 0)) {
        return false;
    }

    memset(frame, 0, wal_frame_size);
    if (!write_u32_le(frame, frame_len, 0, wal_frame_magic) || !write_u16_le(frame, frame_len, 4, wal_frame_revision) ||
        !write_u16_le(frame, frame_len, 6, static_cast<uint16_t>(wal_frame_header_size)) ||
        !write_u64_le(frame, frame_len, 8, header.database_id) || !write_u64_le(frame, frame_len, 16, header.wal_generation) ||
        !write_u64_le(frame, frame_len, 24, header.transaction_sequence) ||
        !write_u16_le(frame, frame_len, 32, header.frame_index) || !write_u16_le(frame, frame_len, 34, header.frame_count) ||
        !write_u16_le(frame, frame_len, 36, header.mutation_count) || !write_u16_le(frame, frame_len, 38, header.flags) ||
        !write_u32_le(frame, frame_len, 40, header.payload_size) ||
        !write_u32_le(frame, frame_len, 44, header.transaction_payload_size) ||
        !write_u32_le(frame, frame_len, 48, header.transaction_checksum)) {
        return false;
    }

    if (payload_len > 0) {
        memcpy(frame + wal_frame_header_size, payload, payload_len);
    }
    const uint32_t payload_checksum = calc_crc32(payload, payload_len);
    if (!write_u32_le(frame, frame_len, 52, payload_checksum)) {
        return false;
    }
    const uint32_t frame_checksum = calc_record_crc(frame, wal_frame_size, wal_frame_checksum_offset);
    return write_u32_le(frame, frame_len, wal_frame_checksum_offset, frame_checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_wal_frame(const uint8_t *frame, size_t frame_len, wal_frame_header *header_out,
                                                         const uint8_t **payload_out)
{
    if (frame == nullptr || header_out == nullptr || payload_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (frame_len < wal_frame_size) {
        return format_status::invalid_size;
    }

    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t header_size = 0;
    uint32_t reserved = 0;
    uint32_t frame_checksum = 0;
    wal_frame_header header = {};
    const bool decoded =
        read_u32_le(frame, frame_len, 0, &magic) && read_u16_le(frame, frame_len, 4, &revision) &&
        read_u16_le(frame, frame_len, 6, &header_size) && read_u64_le(frame, frame_len, 8, &header.database_id) &&
        read_u64_le(frame, frame_len, 16, &header.wal_generation) &&
        read_u64_le(frame, frame_len, 24, &header.transaction_sequence) &&
        read_u16_le(frame, frame_len, 32, &header.frame_index) && read_u16_le(frame, frame_len, 34, &header.frame_count) &&
        read_u16_le(frame, frame_len, 36, &header.mutation_count) && read_u16_le(frame, frame_len, 38, &header.flags) &&
        read_u32_le(frame, frame_len, 40, &header.payload_size) &&
        read_u32_le(frame, frame_len, 44, &header.transaction_payload_size) &&
        read_u32_le(frame, frame_len, 48, &header.transaction_checksum) &&
        read_u32_le(frame, frame_len, 52, &header.payload_checksum) && read_u32_le(frame, frame_len, 56, &reserved) &&
        read_u32_le(frame, frame_len, wal_frame_checksum_offset, &frame_checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (frame_checksum != calc_record_crc(frame, wal_frame_size, wal_frame_checksum_offset)) {
        return format_status::corrupt;
    }
    if (magic != wal_frame_magic) {
        return format_status::invalid_magic;
    }
    if (revision > wal_frame_revision) {
        return format_status::new_version;
    }
    if (revision != wal_frame_revision) {
        return format_status::invalid_revision;
    }
    if (header_size != wal_frame_header_size || header.payload_size > wal_frame_payload_capacity) {
        return format_status::invalid_size;
    }

    const bool final_frame = header.frame_count > 0 && header.frame_index + 1U == header.frame_count;
    if (header.database_id == 0 || header.wal_generation == 0 || header.transaction_sequence == 0 || header.frame_count == 0 ||
        header.frame_index >= header.frame_count || header.mutation_count == 0 ||
        header.mutation_count > max_transaction_mutations || header.transaction_payload_size == 0 ||
        header.payload_size > header.transaction_payload_size || (header.flags & ~wal_frame_flag_commit) != 0 ||
        final_frame != ((header.flags & wal_frame_flag_commit) != 0) || reserved != 0 ||
        !is_zero_range(frame, wal_frame_header_size + header.payload_size, wal_frame_payload_capacity - header.payload_size)) {
        return format_status::corrupt;
    }

    const uint8_t *payload = frame + wal_frame_header_size;
    if (header.payload_checksum != calc_crc32(payload, header.payload_size)) {
        return format_status::corrupt;
    }

    *header_out = header;
    *payload_out = payload;
    return format_status::ok;
}

namespace
{
    bool valid_name_bytes(const uint8_t *name, uint8_t size)
    {
        return on9kvdb_def::validate_bytes(name, size);
    }

    bool valid_table_metadata(const on9kvdb_def::table_metadata &metadata)
    {
        uint64_t data_blocks_bytes = 0;
        uint64_t data_region_end = 0;
        uint64_t index_region_end = 0;
        if (!on9kvdb_def::checked_mul_u64(metadata.data_block_count, metadata.block_size, &data_blocks_bytes) ||
            !on9kvdb_def::checked_add_u64(metadata.data_region_start, data_blocks_bytes, &data_region_end) ||
            !on9kvdb_def::checked_add_u64(metadata.index_offset, metadata.block_size, &index_region_end)) {
            return false;
        }

        return metadata.database_id != 0 && metadata.generation != 0 && metadata.min_sequence != 0 &&
               metadata.max_sequence >= metadata.min_sequence && metadata.slot < on9kvdb_def::max_table_count &&
               metadata.level == 0 && metadata.block_size > on9kvdb_def::table_block_header_size &&
               metadata.block_size % on9kvdb_def::format_alignment == 0 &&
               metadata.data_region_start == on9kvdb_def::table_data_region_offset && metadata.data_block_count > 0 &&
               metadata.index_offset >= data_region_end &&
               (metadata.index_offset - metadata.data_region_start) % metadata.block_size == 0 &&
               metadata.footer_offset == index_region_end && metadata.footer_offset % on9kvdb_def::format_alignment == 0 &&
               metadata.entry_count > 0 && metadata.data_block_count <= metadata.entry_count && metadata.data_bytes > 0 &&
               valid_composite_key(metadata.min_key) && valid_composite_key(metadata.max_key) &&
               on9kvdb_def::compare_composite_key(metadata.min_key, metadata.max_key) <= 0;
    }

    uint32_t revision_for_table_metadata_magic(uint32_t magic)
    {
        if (magic == on9kvdb_def::table_header_magic) {
            return on9kvdb_def::table_header_revision;
        }
        if (magic == on9kvdb_def::table_footer_magic) {
            return on9kvdb_def::table_footer_revision;
        }
        return 0;
    }

    uint32_t align_up_u32(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1U) & ~(alignment - 1U);
    }
}

bool on9kvdb_def::encode_table_metadata(uint8_t *buf, size_t buf_len, uint32_t magic, const table_metadata &metadata)
{
    const uint32_t revision = revision_for_table_metadata_magic(magic);
    if (buf == nullptr || buf_len < table_metadata_size || revision == 0 || !valid_table_metadata(metadata)) {
        return false;
    }

    memset(buf, 0, table_metadata_size);
    if (!write_u32_le(buf, buf_len, 0, magic) || !write_u16_le(buf, buf_len, 4, static_cast<uint16_t>(revision)) ||
        !write_u16_le(buf, buf_len, 6, static_cast<uint16_t>(table_metadata_size)) ||
        !write_u64_le(buf, buf_len, 8, metadata.database_id) || !write_u64_le(buf, buf_len, 16, metadata.generation) ||
        !write_u32_le(buf, buf_len, 24, metadata.slot) || !write_u16_le(buf, buf_len, 28, metadata.level) ||
        !write_u32_le(buf, buf_len, 32, metadata.block_size) || !write_u32_le(buf, buf_len, 36, metadata.data_region_start) ||
        !write_u32_le(buf, buf_len, 40, metadata.data_block_count) || !write_u32_le(buf, buf_len, 44, metadata.index_offset) ||
        !write_u32_le(buf, buf_len, 48, metadata.footer_offset) || !write_u32_le(buf, buf_len, 52, metadata.entry_count) ||
        !write_u32_le(buf, buf_len, 56, metadata.data_bytes) || !write_u32_le(buf, buf_len, 60, metadata.content_checksum) ||
        !write_u64_le(buf, buf_len, 64, metadata.min_sequence) || !write_u64_le(buf, buf_len, 72, metadata.max_sequence)) {
        return false;
    }
    if (!write_u16_le(buf, buf_len, 80, metadata.min_key.namespace_size) ||
        !write_u16_le(buf, buf_len, 82, metadata.min_key.key_size) ||
        !write_u16_le(buf, buf_len, 84, metadata.max_key.namespace_size) ||
        !write_u16_le(buf, buf_len, 86, metadata.max_key.key_size)) {
        return false;
    }
    memcpy(buf + 88, metadata.min_key.namespace_name, metadata.min_key.namespace_size);
    memcpy(buf + 216, metadata.min_key.key, metadata.min_key.key_size);
    memcpy(buf + 344, metadata.max_key.namespace_name, metadata.max_key.namespace_size);
    memcpy(buf + 472, metadata.max_key.key, metadata.max_key.key_size);
    const uint32_t checksum = calc_record_crc(buf, table_metadata_size, table_metadata_checksum_offset);
    return write_u32_le(buf, buf_len, table_metadata_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_table_metadata(const uint8_t *buf, size_t buf_len, uint32_t expected_magic,
                                                              table_metadata *metadata_out)
{
    const uint32_t expected_revision = revision_for_table_metadata_magic(expected_magic);
    if (buf == nullptr || metadata_out == nullptr || expected_revision == 0) {
        return format_status::invalid_argument;
    }
    if (buf_len < table_metadata_size) {
        return format_status::invalid_size;
    }

    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t size = 0;
    uint16_t level = 0;
    uint32_t checksum = 0;
    table_metadata metadata = {};
    const bool decoded =
        read_u32_le(buf, buf_len, 0, &magic) && read_u16_le(buf, buf_len, 4, &revision) && read_u16_le(buf, buf_len, 6, &size) &&
        read_u64_le(buf, buf_len, 8, &metadata.database_id) && read_u64_le(buf, buf_len, 16, &metadata.generation) &&
        read_u32_le(buf, buf_len, 24, &metadata.slot) && read_u16_le(buf, buf_len, 28, &level) &&
        read_u32_le(buf, buf_len, 32, &metadata.block_size) && read_u32_le(buf, buf_len, 36, &metadata.data_region_start) &&
        read_u32_le(buf, buf_len, 40, &metadata.data_block_count) && read_u32_le(buf, buf_len, 44, &metadata.index_offset) &&
        read_u32_le(buf, buf_len, 48, &metadata.footer_offset) && read_u32_le(buf, buf_len, 52, &metadata.entry_count) &&
        read_u32_le(buf, buf_len, 56, &metadata.data_bytes) && read_u32_le(buf, buf_len, 60, &metadata.content_checksum) &&
        read_u64_le(buf, buf_len, 64, &metadata.min_sequence) && read_u64_le(buf, buf_len, 72, &metadata.max_sequence) &&
        read_u16_le(buf, buf_len, 80, &metadata.min_key.namespace_size) &&
        read_u16_le(buf, buf_len, 82, &metadata.min_key.key_size) &&
        read_u16_le(buf, buf_len, 84, &metadata.max_key.namespace_size) &&
        read_u16_le(buf, buf_len, 86, &metadata.max_key.key_size) &&
        read_u32_le(buf, buf_len, table_metadata_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (checksum != calc_record_crc(buf, table_metadata_size, table_metadata_checksum_offset)) {
        return format_status::corrupt;
    }
    if (magic != expected_magic) {
        return format_status::invalid_magic;
    }
    if (revision > expected_revision) {
        return format_status::new_version;
    }
    if (revision != expected_revision) {
        return format_status::invalid_revision;
    }
    if (size != table_metadata_size) {
        return format_status::invalid_size;
    }

    metadata.level = static_cast<uint8_t>(level);
    if (level > UINT8_MAX || metadata.min_key.namespace_size == 0 || metadata.min_key.namespace_size > max_name_len ||
        metadata.min_key.key_size == 0 || metadata.min_key.key_size > max_name_len || metadata.max_key.namespace_size == 0 ||
        metadata.max_key.namespace_size > max_name_len || metadata.max_key.key_size == 0 ||
        metadata.max_key.key_size > max_name_len || !is_zero_range(buf, 30, 2) || !is_zero_range(buf, 600, 36)) {
        return format_status::corrupt;
    }
    memcpy(metadata.min_key.namespace_name, buf + 88, metadata.min_key.namespace_size);
    memcpy(metadata.min_key.key, buf + 216, metadata.min_key.key_size);
    memcpy(metadata.max_key.namespace_name, buf + 344, metadata.max_key.namespace_size);
    memcpy(metadata.max_key.key, buf + 472, metadata.max_key.key_size);
    if (!is_zero_range(buf, 88 + metadata.min_key.namespace_size, max_name_len - metadata.min_key.namespace_size) ||
        !is_zero_range(buf, 216 + metadata.min_key.key_size, max_name_len - metadata.min_key.key_size) ||
        !is_zero_range(buf, 344 + metadata.max_key.namespace_size, max_name_len - metadata.max_key.namespace_size) ||
        !is_zero_range(buf, 472 + metadata.max_key.key_size, max_name_len - metadata.max_key.key_size) ||
        !valid_table_metadata(metadata)) {
        return format_status::corrupt;
    }

    *metadata_out = metadata;
    return format_status::ok;
}

bool on9kvdb_def::table_metadata_equal(const table_metadata &lhs, const table_metadata &rhs)
{
    return lhs.database_id == rhs.database_id && lhs.generation == rhs.generation && lhs.min_sequence == rhs.min_sequence &&
           lhs.max_sequence == rhs.max_sequence && lhs.slot == rhs.slot && lhs.block_size == rhs.block_size &&
           lhs.data_region_start == rhs.data_region_start && lhs.data_block_count == rhs.data_block_count &&
           lhs.index_offset == rhs.index_offset && lhs.footer_offset == rhs.footer_offset && lhs.entry_count == rhs.entry_count &&
           lhs.data_bytes == rhs.data_bytes && lhs.content_checksum == rhs.content_checksum && lhs.level == rhs.level &&
           composite_key_equal(lhs.min_key, rhs.min_key) && composite_key_equal(lhs.max_key, rhs.max_key);
}

bool on9kvdb_def::value_ref_is_valid(const value_ref &reference, uint32_t bank_size)
{
    if (reference.bank_slot >= value_bank_count || reference.bank_generation == 0 || reference.value_size == 0 ||
        reference.value_size > max_value_len || reference.first_chunk_offset < identity_region_size ||
        reference.first_chunk_offset % value_chunk_size != 0 || bank_size <= identity_region_size ||
        reference.first_chunk_offset > bank_size - value_chunk_size) {
        return false;
    }

    const uint64_t chunk_count =
        (static_cast<uint64_t>(reference.value_size) + value_chunk_payload_size - 1U) / value_chunk_payload_size;
    uint64_t bytes = 0;
    uint64_t end = 0;
    return checked_mul_u64(chunk_count, value_chunk_size, &bytes) && checked_add_u64(reference.first_chunk_offset, bytes, &end) &&
           end <= bank_size;
}

bool on9kvdb_def::encode_table_block_header(uint8_t *block, size_t block_len, const table_block_header &header)
{
    if (block == nullptr || block_len < table_block_header_size || header.generation == 0 || header.entry_count == 0 ||
        header.payload_size == 0 || header.payload_size > block_len - table_block_header_size) {
        return false;
    }
    memset(block, 0, table_block_header_size);
    if (!write_u32_le(block, block_len, 0, table_block_magic) || !write_u16_le(block, block_len, 4, table_block_revision) ||
        !write_u16_le(block, block_len, 6, static_cast<uint16_t>(table_block_header_size)) ||
        !write_u64_le(block, block_len, 8, header.generation) || !write_u32_le(block, block_len, 16, header.block_index) ||
        !write_u16_le(block, block_len, 20, header.entry_count) || !write_u32_le(block, block_len, 24, header.payload_size)) {
        return false;
    }
    const uint32_t checksum = calc_record_crc(block, block_len, table_block_checksum_offset);
    return write_u32_le(block, block_len, table_block_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_table_block_header(const uint8_t *block, size_t block_len,
                                                                  table_block_header *header_out)
{
    if (block == nullptr || header_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (block_len < table_block_header_size) {
        return format_status::invalid_size;
    }
    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t size = 0;
    uint16_t reserved = 0;
    uint32_t checksum = 0;
    table_block_header header = {};
    const bool decoded = read_u32_le(block, block_len, 0, &magic) && read_u16_le(block, block_len, 4, &revision) &&
                         read_u16_le(block, block_len, 6, &size) && read_u64_le(block, block_len, 8, &header.generation) &&
                         read_u32_le(block, block_len, 16, &header.block_index) &&
                         read_u16_le(block, block_len, 20, &header.entry_count) && read_u16_le(block, block_len, 22, &reserved) &&
                         read_u32_le(block, block_len, 24, &header.payload_size) &&
                         read_u32_le(block, block_len, table_block_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (checksum != calc_record_crc(block, block_len, table_block_checksum_offset)) {
        return format_status::corrupt;
    }
    if (magic != table_block_magic) {
        return format_status::invalid_magic;
    }
    if (revision > table_block_revision) {
        return format_status::new_version;
    }
    if (revision != table_block_revision) {
        return format_status::invalid_revision;
    }
    if (size != table_block_header_size || header.payload_size > block_len - table_block_header_size) {
        return format_status::invalid_size;
    }
    if (header.generation == 0 || header.entry_count == 0 || header.payload_size == 0 || reserved != 0 ||
        !is_zero_range(block, 28, 32) ||
        !is_zero_range(block, table_block_header_size + header.payload_size,
                       block_len - table_block_header_size - header.payload_size)) {
        return format_status::corrupt;
    }
    *header_out = header;
    return format_status::ok;
}

bool on9kvdb_def::encode_table_index_header(uint8_t *block, size_t block_len, const table_index_header &header)
{
    if (block == nullptr || block_len < table_index_header_size || header.generation == 0 || header.entry_count == 0 ||
        header.payload_size == 0 || header.payload_size > block_len - table_index_header_size || header.data_block_count == 0 ||
        header.entry_count != header.data_block_count) {
        return false;
    }
    memset(block, 0, table_index_header_size);
    if (!write_u32_le(block, block_len, 0, table_index_magic) || !write_u16_le(block, block_len, 4, table_index_revision) ||
        !write_u16_le(block, block_len, 6, static_cast<uint16_t>(table_index_header_size)) ||
        !write_u64_le(block, block_len, 8, header.generation) || !write_u32_le(block, block_len, 16, header.entry_count) ||
        !write_u32_le(block, block_len, 20, header.payload_size) ||
        !write_u32_le(block, block_len, 24, header.data_block_count)) {
        return false;
    }
    const uint32_t checksum = calc_record_crc(block, block_len, table_index_checksum_offset);
    return write_u32_le(block, block_len, table_index_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_table_index_header(const uint8_t *block, size_t block_len,
                                                                  table_index_header *header_out)
{
    if (block == nullptr || header_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (block_len < table_index_header_size) {
        return format_status::invalid_size;
    }
    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t size = 0;
    uint32_t checksum = 0;
    table_index_header header = {};
    const bool decoded = read_u32_le(block, block_len, 0, &magic) && read_u16_le(block, block_len, 4, &revision) &&
                         read_u16_le(block, block_len, 6, &size) && read_u64_le(block, block_len, 8, &header.generation) &&
                         read_u32_le(block, block_len, 16, &header.entry_count) &&
                         read_u32_le(block, block_len, 20, &header.payload_size) &&
                         read_u32_le(block, block_len, 24, &header.data_block_count) &&
                         read_u32_le(block, block_len, table_index_checksum_offset, &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (checksum != calc_record_crc(block, block_len, table_index_checksum_offset)) {
        return format_status::corrupt;
    }
    if (magic != table_index_magic) {
        return format_status::invalid_magic;
    }
    if (revision > table_index_revision) {
        return format_status::new_version;
    }
    if (revision != table_index_revision) {
        return format_status::invalid_revision;
    }
    if (size != table_index_header_size || header.payload_size > block_len - table_index_header_size) {
        return format_status::invalid_size;
    }
    if (header.generation == 0 || header.entry_count == 0 || header.payload_size == 0 || header.data_block_count == 0 ||
        header.entry_count != header.data_block_count || !is_zero_range(block, 28, 32) ||
        !is_zero_range(block, table_index_header_size + header.payload_size,
                       block_len - table_index_header_size - header.payload_size)) {
        return format_status::corrupt;
    }
    *header_out = header;
    return format_status::ok;
}

bool on9kvdb_def::encode_table_entry(uint8_t *buf, size_t buf_len, size_t offset, const table_entry &entry, size_t *size_out)
{
    if (buf == nullptr || size_out == nullptr || entry.transaction_sequence == 0 ||
        !valid_name_bytes(entry.namespace_name, entry.namespace_size) || !valid_name_bytes(entry.key, entry.key_size) ||
        (entry.value == nullptr && entry.value_size != 0 && (entry.flags & table_entry_flag_external_value) == 0) ||
        entry.value_size > max_value_len || entry.reserved0 != 0 ||
        (entry.flags & ~(table_entry_flag_tombstone | table_entry_flag_external_value)) != 0 ||
        (((entry.flags & table_entry_flag_tombstone) != 0) &&
         (entry.value_size != 0 || (entry.flags & table_entry_flag_external_value) != 0)) ||
        (((entry.flags & table_entry_flag_external_value) != 0) &&
         !value_ref_is_valid(entry.external_value, static_cast<uint32_t>(max_storage_object_size)))) {
        return false;
    }
    const bool external = (entry.flags & table_entry_flag_external_value) != 0;
    const uint32_t encoded_value_size = external ? value_ref_encoded_size : entry.value_size;
    const uint32_t meaningful_size = table_entry_header_size + entry.namespace_size + entry.key_size + encoded_value_size;
    const uint32_t total_size = align_up_u32(meaningful_size, 8);
    if (!is_range_valid(buf_len, offset, total_size)) {
        return false;
    }
    memset(buf + offset, 0, total_size);
    if (!write_u32_le(buf, buf_len, offset, total_size) || !write_u32_le(buf, buf_len, offset + 4, entry.value_size) ||
        !write_u64_le(buf, buf_len, offset + 8, entry.transaction_sequence)) {
        return false;
    }
    buf[offset + 16] = entry.namespace_size;
    buf[offset + 17] = entry.key_size;
    buf[offset + 18] = entry.reserved0;
    buf[offset + 19] = entry.flags;
    memcpy(buf + offset + table_entry_header_size, entry.namespace_name, entry.namespace_size);
    memcpy(buf + offset + table_entry_header_size + entry.namespace_size, entry.key, entry.key_size);
    const size_t value_offset = offset + table_entry_header_size + entry.namespace_size + entry.key_size;
    if (external) {
        uint8_t *encoded_ref = buf + value_offset;
        encoded_ref[0] = entry.external_value.bank_slot;
        if (!write_u64_le(encoded_ref, value_ref_encoded_size, 4, entry.external_value.bank_generation) ||
            !write_u32_le(encoded_ref, value_ref_encoded_size, 12, entry.external_value.first_chunk_offset) ||
            !write_u32_le(encoded_ref, value_ref_encoded_size, 16, entry.external_value.value_checksum)) {
            return false;
        }
    } else if (entry.value_size > 0) {
        memcpy(buf + value_offset, entry.value, entry.value_size);
    }
    // A table block checksum detects torn/corrupt blocks, while this record checksum makes an individual key/value record
    // independently verifiable before its descriptor or inline bytes are trusted.
    const uint32_t checksum = calc_record_crc(buf + offset, total_size, 20);
    if (!write_u32_le(buf, buf_len, offset + 20, checksum)) {
        return false;
    }
    *size_out = total_size;
    return true;
}

on9kvdb_def::format_status on9kvdb_def::decode_table_entry(const uint8_t *buf, size_t buf_len, size_t offset,
                                                           table_entry *entry_out)
{
    if (buf == nullptr || entry_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (!is_range_valid(buf_len, offset, table_entry_header_size)) {
        return format_status::invalid_size;
    }
    table_entry entry = {};
    if (!read_u32_le(buf, buf_len, offset, &entry.total_size) || !read_u32_le(buf, buf_len, offset + 4, &entry.value_size) ||
        !read_u64_le(buf, buf_len, offset + 8, &entry.transaction_sequence)) {
        return format_status::invalid_size;
    }
    entry.namespace_size = buf[offset + 16];
    entry.key_size = buf[offset + 17];
    entry.reserved0 = buf[offset + 18];
    entry.flags = buf[offset + 19];
    uint32_t record_checksum = 0;
    const bool external = (entry.flags & table_entry_flag_external_value) != 0;
    const uint32_t encoded_value_size = external ? value_ref_encoded_size : entry.value_size;
    const uint32_t meaningful_size = table_entry_header_size + entry.namespace_size + entry.key_size + encoded_value_size;
    if (entry.transaction_sequence == 0 || entry.total_size < meaningful_size || entry.total_size % 8 != 0 ||
        !is_range_valid(buf_len, offset, entry.total_size) || entry.value_size > max_value_len || entry.reserved0 != 0 ||
        (entry.flags & ~(table_entry_flag_tombstone | table_entry_flag_external_value)) != 0 ||
        (((entry.flags & table_entry_flag_tombstone) != 0) && (entry.value_size != 0 || external)) ||
        !read_u32_le(buf, buf_len, offset + 20, &record_checksum) ||
        record_checksum != calc_record_crc(buf + offset, entry.total_size, 20)) {
        return format_status::corrupt;
    }
    entry.namespace_name = buf + offset + table_entry_header_size;
    entry.key = entry.namespace_name + entry.namespace_size;
    entry.value = entry.key + entry.key_size;
    if (external) {
        const uint8_t *encoded_ref = entry.value;
        entry.external_value = {};
        entry.external_value.bank_slot = encoded_ref[0];
        if (!is_zero_range(encoded_ref, 1, 3) ||
            !read_u64_le(encoded_ref, value_ref_encoded_size, 4, &entry.external_value.bank_generation) ||
            !read_u32_le(encoded_ref, value_ref_encoded_size, 12, &entry.external_value.first_chunk_offset) ||
            !read_u32_le(encoded_ref, value_ref_encoded_size, 16, &entry.external_value.value_checksum) ||
            !is_zero_range(encoded_ref, 20, 4)) {
            return format_status::corrupt;
        }
        entry.external_value.value_size = entry.value_size;
        if (!value_ref_is_valid(entry.external_value, static_cast<uint32_t>(max_storage_object_size))) {
            return format_status::corrupt;
        }
        entry.value = nullptr;
    }
    if (!valid_name_bytes(entry.namespace_name, entry.namespace_size) || !valid_name_bytes(entry.key, entry.key_size) ||
        !is_zero_range(buf, offset + meaningful_size, entry.total_size - meaningful_size)) {
        return format_status::corrupt;
    }
    *entry_out = entry;
    return format_status::ok;
}

bool on9kvdb_def::encode_table_index_entry(uint8_t *buf, size_t buf_len, size_t offset, const table_index_entry &entry,
                                           size_t *size_out)
{
    if (buf == nullptr || size_out == nullptr || entry.first_sequence == 0 ||
        !valid_name_bytes(entry.namespace_name, entry.namespace_size) || !valid_name_bytes(entry.key, entry.key_size) ||
        entry.block_offset < table_data_region_offset || entry.block_offset % format_alignment != 0) {
        return false;
    }
    const uint32_t meaningful_size = table_index_entry_header_size + entry.namespace_size + entry.key_size;
    const uint32_t total_size = align_up_u32(meaningful_size, 4);
    if (!is_range_valid(buf_len, offset, total_size) || total_size > UINT16_MAX) {
        return false;
    }
    memset(buf + offset, 0, total_size);
    if (!write_u16_le(buf, buf_len, offset, static_cast<uint16_t>(total_size)) ||
        !write_u32_le(buf, buf_len, offset + 4, entry.block_offset) ||
        !write_u64_le(buf, buf_len, offset + 8, entry.first_sequence)) {
        return false;
    }
    buf[offset + 2] = entry.namespace_size;
    buf[offset + 3] = entry.key_size;
    memcpy(buf + offset + table_index_entry_header_size, entry.namespace_name, entry.namespace_size);
    memcpy(buf + offset + table_index_entry_header_size + entry.namespace_size, entry.key, entry.key_size);
    *size_out = total_size;
    return true;
}

on9kvdb_def::format_status on9kvdb_def::decode_table_index_entry(const uint8_t *buf, size_t buf_len, size_t offset,
                                                                 table_index_entry *entry_out)
{
    if (buf == nullptr || entry_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (!is_range_valid(buf_len, offset, table_index_entry_header_size)) {
        return format_status::invalid_size;
    }
    table_index_entry entry = {};
    if (!read_u16_le(buf, buf_len, offset, &entry.total_size) || !read_u32_le(buf, buf_len, offset + 4, &entry.block_offset) ||
        !read_u64_le(buf, buf_len, offset + 8, &entry.first_sequence)) {
        return format_status::invalid_size;
    }
    entry.namespace_size = buf[offset + 2];
    entry.key_size = buf[offset + 3];
    const uint32_t meaningful_size = table_index_entry_header_size + entry.namespace_size + entry.key_size;
    if (entry.total_size < meaningful_size || entry.total_size % 4 != 0 || !is_range_valid(buf_len, offset, entry.total_size) ||
        entry.first_sequence == 0 || entry.block_offset < table_data_region_offset ||
        entry.block_offset % format_alignment != 0) {
        return format_status::corrupt;
    }
    entry.namespace_name = buf + offset + table_index_entry_header_size;
    entry.key = entry.namespace_name + entry.namespace_size;
    if (!valid_name_bytes(entry.namespace_name, entry.namespace_size) || !valid_name_bytes(entry.key, entry.key_size) ||
        !is_zero_range(buf, offset + meaningful_size, entry.total_size - meaningful_size)) {
        return format_status::corrupt;
    }
    *entry_out = entry;
    return format_status::ok;
}

bool on9kvdb_def::encode_value_chunk(uint8_t *chunk, size_t chunk_size, const value_chunk_header &header, const uint8_t *payload)
{
    const bool final = header.value_offset <= header.value_size && header.payload_size == header.value_size - header.value_offset;
    if (chunk == nullptr || chunk_size < value_chunk_size || (payload == nullptr && header.payload_size != 0) ||
        header.database_id == 0 || header.bank_generation == 0 || header.first_chunk_offset < identity_region_size ||
        header.first_chunk_offset % value_chunk_size != 0 || header.value_size == 0 || header.value_size > max_value_len ||
        header.value_offset >= header.value_size || header.payload_size == 0 || header.payload_size > value_chunk_payload_size ||
        header.payload_size > header.value_size - header.value_offset || (header.flags & ~value_chunk_flag_final) != 0 ||
        final != ((header.flags & value_chunk_flag_final) != 0)) {
        return false;
    }

    // The streaming writer deliberately stages a payload at the beginning of this frame.  Keep that payload intact when
    // encoding in place; using memset(chunk, ...) first would turn every persisted value chunk into zeroes.
    const uintptr_t chunk_start = reinterpret_cast<uintptr_t>(chunk);
    const uintptr_t chunk_end = chunk_start + value_chunk_size;
    const uintptr_t payload_start = reinterpret_cast<uintptr_t>(payload);
    const bool payload_is_in_chunk =
        payload_start >= chunk_start + value_chunk_header_size && payload_start <= chunk_end - header.payload_size;
    if (payload_is_in_chunk) {
        memset(chunk, 0, value_chunk_header_size);
        memset(chunk + value_chunk_header_size + header.payload_size, 0, value_chunk_payload_size - header.payload_size);
    } else {
        memset(chunk, 0, value_chunk_size);
    }
    if (!write_u32_le(chunk, chunk_size, 0, value_chunk_magic) || !write_u16_le(chunk, chunk_size, 4, value_chunk_revision) ||
        !write_u16_le(chunk, chunk_size, 6, value_chunk_header_size) || !write_u64_le(chunk, chunk_size, 8, header.database_id) ||
        !write_u64_le(chunk, chunk_size, 16, header.bank_generation) ||
        !write_u32_le(chunk, chunk_size, 24, header.first_chunk_offset) ||
        !write_u32_le(chunk, chunk_size, 28, header.value_size) || !write_u32_le(chunk, chunk_size, 32, header.value_offset) ||
        !write_u16_le(chunk, chunk_size, 36, header.payload_size) || !write_u16_le(chunk, chunk_size, 38, header.flags)) {
        return false;
    }
    if (header.payload_size > 0) {
        memcpy(chunk + value_chunk_header_size, payload, header.payload_size);
    }
    const uint32_t payload_checksum = calc_crc32(chunk + value_chunk_header_size, header.payload_size);
    if (!write_u32_le(chunk, chunk_size, 40, payload_checksum)) {
        return false;
    }
    const uint32_t record_checksum = calc_record_crc(chunk, value_chunk_size, 60);
    return write_u32_le(chunk, chunk_size, 60, record_checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_value_chunk(const uint8_t *chunk, size_t chunk_size,
                                                           value_chunk_header *header_out, const uint8_t **payload_out)
{
    if (chunk == nullptr || header_out == nullptr || payload_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (chunk_size < value_chunk_size) {
        return format_status::invalid_size;
    }

    uint32_t magic = 0;
    uint16_t revision = 0;
    uint16_t header_size = 0;
    uint32_t record_checksum = 0;
    value_chunk_header header = {};
    const bool decoded =
        read_u32_le(chunk, chunk_size, 0, &magic) && read_u16_le(chunk, chunk_size, 4, &revision) &&
        read_u16_le(chunk, chunk_size, 6, &header_size) && read_u64_le(chunk, chunk_size, 8, &header.database_id) &&
        read_u64_le(chunk, chunk_size, 16, &header.bank_generation) &&
        read_u32_le(chunk, chunk_size, 24, &header.first_chunk_offset) &&
        read_u32_le(chunk, chunk_size, 28, &header.value_size) && read_u32_le(chunk, chunk_size, 32, &header.value_offset) &&
        read_u16_le(chunk, chunk_size, 36, &header.payload_size) && read_u16_le(chunk, chunk_size, 38, &header.flags) &&
        read_u32_le(chunk, chunk_size, 40, &header.payload_checksum) && read_u32_le(chunk, chunk_size, 60, &record_checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }
    if (record_checksum != calc_record_crc(chunk, value_chunk_size, 60)) {
        return format_status::corrupt;
    }
    if (magic != value_chunk_magic) {
        return format_status::invalid_magic;
    }
    if (revision > value_chunk_revision) {
        return format_status::new_version;
    }
    if (revision != value_chunk_revision) {
        return format_status::invalid_revision;
    }
    const bool final = header.value_offset <= header.value_size && header.payload_size == header.value_size - header.value_offset;
    if (header_size != value_chunk_header_size || header.database_id == 0 || header.bank_generation == 0 ||
        header.first_chunk_offset < identity_region_size || header.first_chunk_offset % value_chunk_size != 0 ||
        header.value_size == 0 || header.value_size > max_value_len || header.value_offset >= header.value_size ||
        header.payload_size == 0 || header.payload_size > value_chunk_payload_size ||
        header.payload_size > header.value_size - header.value_offset || (header.flags & ~value_chunk_flag_final) != 0 ||
        final != ((header.flags & value_chunk_flag_final) != 0) || !is_zero_range(chunk, 44, 16) ||
        !is_zero_range(chunk, value_chunk_header_size + header.payload_size, value_chunk_payload_size - header.payload_size) ||
        header.payload_checksum != calc_crc32(chunk + value_chunk_header_size, header.payload_size)) {
        return format_status::corrupt;
    }
    *header_out = header;
    *payload_out = chunk + value_chunk_header_size;
    return format_status::ok;
}
