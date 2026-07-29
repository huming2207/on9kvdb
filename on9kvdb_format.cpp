#include <cstring>
#include <limits>

#include "on9kvdb_defs.hpp"

namespace
{
    static const constexpr uint32_t crc32_polynomial = 0xedb88320UL;

    constexpr uint32_t make_crc32_lut_entry(uint32_t value)
    {
        for (uint8_t bit = 0; bit < 8; bit += 1) {
            value = (value & 1U) ?
                ((value >> 1U) ^ crc32_polynomial) :
                (value >> 1U);
        }

        return value;
    }

    struct crc32_lut {
        uint32_t values[256] = {};

        constexpr crc32_lut()
        {
            for (size_t idx = 0; idx < 256; idx += 1) {
                values[idx] =
                    make_crc32_lut_entry(static_cast<uint32_t>(idx));
            }
        }
    };

    static const constexpr crc32_lut crc32_table = {};

    constexpr uint32_t crc32_constexpr(const char *buf, size_t len)
    {
        uint32_t crc = UINT32_MAX;
        for (size_t idx = 0; idx < len; idx += 1) {
            const uint8_t table_idx = static_cast<uint8_t>(
                crc ^ static_cast<uint8_t>(buf[idx]));
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
        uint32_t crc = on9kvdb_def::calc_crc32_update(
            UINT32_MAX, buf,
            on9kvdb_def::file_prefix_checksum_offset);
        const uint8_t zeroes[sizeof(uint32_t)] = {};
        crc = on9kvdb_def::calc_crc32_update(
            crc, zeroes, sizeof(zeroes));
        return ~crc;
    }

    uint32_t calc_record_crc(const uint8_t *buf, size_t len,
                             size_t checksum_offset)
    {
        uint32_t crc = on9kvdb_def::calc_crc32_update(
            UINT32_MAX, buf, checksum_offset);
        const uint8_t zeroes[sizeof(uint32_t)] = {};
        crc = on9kvdb_def::calc_crc32_update(
            crc, zeroes, sizeof(zeroes));
        const size_t suffix_offset =
            checksum_offset + sizeof(uint32_t);
        crc = on9kvdb_def::calc_crc32_update(
            crc, buf + suffix_offset, len - suffix_offset);
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
        default:
            return 0;
        }
    }

    bool is_zero_range(
        const uint8_t *buf, size_t offset, size_t len)
    {
        for (size_t idx = 0; idx < len; idx += 1) {
            if (buf[offset + idx] != 0) {
                return false;
            }
        }
        return true;
    }
}

bool on9kvdb_def::checked_add_size(
    size_t lhs, size_t rhs, size_t *result_out)
{
    if (result_out == nullptr ||
        rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }

    *result_out = lhs + rhs;
    return true;
}

bool on9kvdb_def::checked_mul_size(
    size_t lhs, size_t rhs, size_t *result_out)
{
    if (result_out == nullptr ||
        (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)) {
        return false;
    }

    *result_out = lhs * rhs;
    return true;
}

bool on9kvdb_def::checked_align_up_size(
    size_t value, size_t alignment, size_t *result_out)
{
    if (result_out == nullptr || alignment == 0 ||
        (alignment & (alignment - 1U)) != 0) {
        return false;
    }

    size_t adjusted = 0;
    if (!checked_add_size(value, alignment - 1U, &adjusted)) {
        return false;
    }

    *result_out = adjusted & ~(alignment - 1U);
    return true;
}

bool on9kvdb_def::checked_add_u64(
    uint64_t lhs, uint64_t rhs, uint64_t *result_out)
{
    if (result_out == nullptr || rhs > UINT64_MAX - lhs) {
        return false;
    }

    *result_out = lhs + rhs;
    return true;
}

bool on9kvdb_def::checked_mul_u64(
    uint64_t lhs, uint64_t rhs, uint64_t *result_out)
{
    if (result_out == nullptr ||
        (lhs != 0 && rhs > UINT64_MAX / lhs)) {
        return false;
    }

    *result_out = lhs * rhs;
    return true;
}

bool on9kvdb_def::write_u16_le(
    uint8_t *buf, size_t buf_len, size_t offset, uint16_t value)
{
    if (buf == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    buf[offset] = static_cast<uint8_t>(value);
    buf[offset + 1] = static_cast<uint8_t>(value >> 8U);
    return true;
}

bool on9kvdb_def::write_u32_le(
    uint8_t *buf, size_t buf_len, size_t offset, uint32_t value)
{
    if (buf == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        buf[offset + idx] =
            static_cast<uint8_t>(value >> (idx * 8U));
    }
    return true;
}

bool on9kvdb_def::write_u64_le(
    uint8_t *buf, size_t buf_len, size_t offset, uint64_t value)
{
    if (buf == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(value))) {
        return false;
    }

    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        buf[offset + idx] =
            static_cast<uint8_t>(value >> (idx * 8U));
    }
    return true;
}

bool on9kvdb_def::read_u16_le(
    const uint8_t *buf, size_t buf_len, size_t offset,
    uint16_t *value_out)
{
    if (buf == nullptr || value_out == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    *value_out =
        static_cast<uint16_t>(buf[offset]) |
        static_cast<uint16_t>(
            static_cast<uint16_t>(buf[offset + 1]) << 8U);
    return true;
}

bool on9kvdb_def::read_u32_le(
    const uint8_t *buf, size_t buf_len, size_t offset,
    uint32_t *value_out)
{
    if (buf == nullptr || value_out == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    uint32_t value = 0;
    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        value |= static_cast<uint32_t>(buf[offset + idx]) <<
                 (idx * 8U);
    }
    *value_out = value;
    return true;
}

bool on9kvdb_def::read_u64_le(
    const uint8_t *buf, size_t buf_len, size_t offset,
    uint64_t *value_out)
{
    if (buf == nullptr || value_out == nullptr ||
        !is_range_valid(buf_len, offset, sizeof(*value_out))) {
        return false;
    }

    uint64_t value = 0;
    for (size_t idx = 0; idx < sizeof(value); idx += 1) {
        value |= static_cast<uint64_t>(buf[offset + idx]) <<
                 (idx * 8U);
    }
    *value_out = value;
    return true;
}

uint32_t on9kvdb_def::calc_crc32_update(
    uint32_t crc, const uint8_t *buf, size_t len)
{
    if (buf == nullptr && len != 0) {
        return crc;
    }

    for (size_t idx = 0; idx < len; idx += 1) {
        const uint8_t table_idx =
            static_cast<uint8_t>(crc ^ buf[idx]);
        crc = crc32_table.values[table_idx] ^ (crc >> 8U);
    }
    return crc;
}

uint32_t on9kvdb_def::calc_crc32(const uint8_t *buf, size_t len)
{
    return ~calc_crc32_update(UINT32_MAX, buf, len);
}

bool on9kvdb_def::validate_name(
    const char *name, size_t *length_out)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    size_t len = 0;
    while (len <= max_name_len && name[len] != '\0') {
        len += 1;
    }
    if (len == 0 || len > max_name_len) {
        return false;
    }

    if (length_out != nullptr) {
        *length_out = len;
    }
    return true;
}

uint32_t on9kvdb_def::make_handle_value(
    uint16_t slot, uint16_t generation)
{
    if (slot == UINT16_MAX || generation == 0) {
        return 0;
    }

    return (static_cast<uint32_t>(generation) << 16U) |
           (static_cast<uint32_t>(slot) + 1U);
}

bool on9kvdb_def::decode_handle_value(
    uint32_t value, uint16_t *slot_out, uint16_t *generation_out)
{
    if (slot_out == nullptr || generation_out == nullptr) {
        return false;
    }

    const uint16_t slot_token =
        static_cast<uint16_t>(value & UINT16_MAX);
    const uint16_t generation =
        static_cast<uint16_t>(value >> 16U);
    if (slot_token == 0 || generation == 0) {
        return false;
    }

    *slot_out = static_cast<uint16_t>(slot_token - 1U);
    *generation_out = generation;
    return true;
}

bool on9kvdb_def::is_handle_value(
    uint32_t value, uint16_t slot, uint16_t generation)
{
    return value != 0 && value == make_handle_value(slot, generation);
}

bool on9kvdb_def::encode_file_prefix(
    uint8_t *buf, size_t buf_len, uint32_t magic,
    file_kind kind, uint16_t flags, uint64_t generation,
    uint32_t payload_size)
{
    if (buf == nullptr || buf_len < file_prefix_size ||
        magic == 0 || kind == file_kind::invalid || generation == 0) {
        return false;
    }

    memset(buf, 0, file_prefix_size);
    const bool encoded =
        write_u32_le(buf, buf_len, 0, magic) &&
        write_u16_le(buf, buf_len, 4, storage_revision) &&
        write_u16_le(buf, buf_len, 6,
                     static_cast<uint16_t>(file_prefix_size)) &&
        write_u16_le(buf, buf_len, 8,
                     static_cast<uint16_t>(kind)) &&
        write_u16_le(buf, buf_len, 10, flags) &&
        write_u64_le(buf, buf_len, 12, generation) &&
        write_u32_le(buf, buf_len, 20, payload_size);
    if (!encoded) {
        return false;
    }

    const uint32_t checksum = calc_file_prefix_crc(buf);
    return write_u32_le(buf, buf_len, file_prefix_checksum_offset,
                        checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_file_prefix(
    const uint8_t *buf, size_t buf_len, uint32_t expected_magic,
    file_kind expected_kind, decoded_file_prefix *prefix_out)
{
    if (buf == nullptr || prefix_out == nullptr ||
        expected_magic == 0 || expected_kind == file_kind::invalid) {
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
    const bool decoded =
        read_u32_le(buf, buf_len, 0, &magic) &&
        read_u16_le(buf, buf_len, 4, &revision) &&
        read_u16_le(buf, buf_len, 6, &size) &&
        read_u16_le(buf, buf_len, 8, &kind_raw) &&
        read_u16_le(buf, buf_len, 10, &flags) &&
        read_u64_le(buf, buf_len, 12, &generation) &&
        read_u32_le(buf, buf_len, 20, &payload_size) &&
        read_u32_le(buf, buf_len, file_prefix_checksum_offset,
                    &checksum);
    if (!decoded) {
        return format_status::invalid_size;
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
    if (checksum != calc_file_prefix_crc(buf)) {
        return format_status::corrupt;
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

bool on9kvdb_def::validate_storage_geometry(
    const storage_geometry &geometry)
{
    if (geometry.provisioned_size == 0 ||
        geometry.max_live_bytes == 0 ||
        geometry.max_live_bytes > geometry.provisioned_size ||
        geometry.manifest_size != manifest_file_size ||
        geometry.wal_size <= identity_region_size ||
        geometry.wal_count != wal_file_count ||
        geometry.table_size <= identity_region_size ||
        geometry.table_count < 2 ||
        geometry.alignment != format_alignment ||
        geometry.manifest_size % format_alignment != 0 ||
        geometry.wal_size % format_alignment != 0 ||
        geometry.table_size % format_alignment != 0) {
        return false;
    }

    uint64_t wal_bytes = 0;
    uint64_t table_bytes = 0;
    uint64_t table_payload_bytes = 0;
    uint64_t calculated_size = geometry.manifest_size;
    return checked_mul_u64(
               geometry.wal_size, geometry.wal_count, &wal_bytes) &&
           checked_mul_u64(
               geometry.table_size, geometry.table_count, &table_bytes) &&
           checked_mul_u64(
               geometry.table_size - identity_region_size,
               geometry.table_count, &table_payload_bytes) &&
           checked_add_u64(
               calculated_size, wal_bytes, &calculated_size) &&
           checked_add_u64(
               calculated_size, table_bytes, &calculated_size) &&
           calculated_size == geometry.provisioned_size &&
           geometry.max_live_bytes <= table_payload_bytes;
}

bool on9kvdb_def::storage_geometry_equal(
    const storage_geometry &lhs, const storage_geometry &rhs)
{
    return lhs.provisioned_size == rhs.provisioned_size &&
           lhs.max_live_bytes == rhs.max_live_bytes &&
           lhs.manifest_size == rhs.manifest_size &&
           lhs.wal_size == rhs.wal_size &&
           lhs.wal_count == rhs.wal_count &&
           lhs.table_size == rhs.table_size &&
           lhs.table_count == rhs.table_count &&
           lhs.alignment == rhs.alignment;
}

bool on9kvdb_def::encode_manifest_record(
    uint8_t *buf, size_t buf_len, const manifest_record &record)
{
    if (buf == nullptr || buf_len < manifest_record_size ||
        record.generation == 0 || record.database_id == 0 ||
        (record.state != manifest_state_provisioning_owned &&
         record.state != manifest_state_ready) ||
        !validate_storage_geometry(record.geometry)) {
        return false;
    }

    memset(buf, 0, manifest_record_size);
    if (!encode_file_prefix(
            buf, buf_len, manifest_magic, file_kind::manifest, 0,
            record.generation,
            static_cast<uint32_t>(
                manifest_record_size - file_prefix_size)) ||
        !write_u64_le(buf, buf_len, 28, record.database_id) ||
        !write_u16_le(buf, buf_len, 36, record.state) ||
        !write_u16_le(buf, buf_len, 38, geometry_revision) ||
        !write_u32_le(buf, buf_len, 40, record.geometry.alignment) ||
        !write_u32_le(buf, buf_len, 44, record.geometry.manifest_size) ||
        !write_u32_le(buf, buf_len, 48, record.geometry.wal_size) ||
        !write_u32_le(buf, buf_len, 52, record.geometry.wal_count) ||
        !write_u32_le(buf, buf_len, 56, record.geometry.table_size) ||
        !write_u32_le(buf, buf_len, 60, record.geometry.table_count) ||
        !write_u64_le(buf, buf_len, 64,
                      record.geometry.max_live_bytes) ||
        !write_u64_le(buf, buf_len, 72,
                      record.geometry.provisioned_size)) {
        return false;
    }

    const uint32_t checksum = calc_record_crc(
        buf, manifest_record_size, manifest_record_checksum_offset);
    return write_u32_le(buf, buf_len,
                        manifest_record_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_manifest_record(
    const uint8_t *buf, size_t buf_len, manifest_record *record_out)
{
    if (buf == nullptr || record_out == nullptr) {
        return format_status::invalid_argument;
    }
    if (buf_len < manifest_record_size) {
        return format_status::invalid_size;
    }

    decoded_file_prefix prefix = {};
    const format_status prefix_status = decode_file_prefix(
        buf, buf_len, manifest_magic, file_kind::manifest, &prefix);
    if (prefix_status != format_status::ok) {
        return prefix_status;
    }
    if (prefix.payload_size != manifest_record_size - file_prefix_size) {
        return format_status::invalid_size;
    }

    manifest_record record = {};
    uint16_t geometry_format = 0;
    uint32_t checksum = 0;
    const bool decoded =
        read_u64_le(buf, buf_len, 28, &record.database_id) &&
        read_u16_le(buf, buf_len, 36, &record.state) &&
        read_u16_le(buf, buf_len, 38, &geometry_format) &&
        read_u32_le(buf, buf_len, 40,
                    &record.geometry.alignment) &&
        read_u32_le(buf, buf_len, 44,
                    &record.geometry.manifest_size) &&
        read_u32_le(buf, buf_len, 48,
                    &record.geometry.wal_size) &&
        read_u32_le(buf, buf_len, 52,
                    &record.geometry.wal_count) &&
        read_u32_le(buf, buf_len, 56,
                    &record.geometry.table_size) &&
        read_u32_le(buf, buf_len, 60,
                    &record.geometry.table_count) &&
        read_u64_le(buf, buf_len, 64,
                    &record.geometry.max_live_bytes) &&
        read_u64_le(buf, buf_len, 72,
                    &record.geometry.provisioned_size) &&
        read_u32_le(buf, buf_len, manifest_record_checksum_offset,
                    &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }

    record.generation = prefix.generation;
    if (record.database_id == 0 ||
        (record.state != manifest_state_provisioning_owned &&
         record.state != manifest_state_ready) ||
        (record.state == manifest_state_ready &&
         record.generation < 3) ||
        geometry_format != geometry_revision ||
        !is_zero_range(buf, 80, 12) ||
        !validate_storage_geometry(record.geometry)) {
        return format_status::corrupt;
    }
    if (checksum != calc_record_crc(
            buf, manifest_record_size,
            manifest_record_checksum_offset)) {
        return format_status::corrupt;
    }

    *record_out = record;
    return format_status::ok;
}

bool on9kvdb_def::encode_file_identity(
    uint8_t *buf, size_t buf_len, const file_identity &identity)
{
    const uint32_t magic = magic_for_kind(identity.kind);
    if (buf == nullptr || buf_len < file_identity_size ||
        magic == 0 || identity.kind == file_kind::manifest ||
        identity.generation == 0 || identity.database_id == 0 ||
        identity.file_size <= identity_region_size ||
        identity.file_size > max_fat32_file_size ||
        identity.file_size % format_alignment != 0) {
        return false;
    }

    memset(buf, 0, file_identity_size);
    if (!encode_file_prefix(
            buf, buf_len, magic, identity.kind,
            file_prefix_flag_identity, identity.generation,
            static_cast<uint32_t>(
                file_identity_size - file_prefix_size)) ||
        !write_u64_le(buf, buf_len, 28, identity.database_id) ||
        !write_u64_le(buf, buf_len, 36, identity.file_size) ||
        !write_u32_le(buf, buf_len, 44, identity.slot)) {
        return false;
    }

    const uint32_t checksum = calc_record_crc(
        buf, file_identity_size, file_identity_checksum_offset);
    return write_u32_le(
        buf, buf_len, file_identity_checksum_offset, checksum);
}

on9kvdb_def::format_status on9kvdb_def::decode_file_identity(
    const uint8_t *buf, size_t buf_len, file_kind expected_kind,
    file_identity *identity_out)
{
    const uint32_t magic = magic_for_kind(expected_kind);
    if (buf == nullptr || identity_out == nullptr ||
        magic == 0 || expected_kind == file_kind::manifest) {
        return format_status::invalid_argument;
    }
    if (buf_len < file_identity_size) {
        return format_status::invalid_size;
    }

    decoded_file_prefix prefix = {};
    const format_status prefix_status = decode_file_prefix(
        buf, buf_len, magic, expected_kind, &prefix);
    if (prefix_status != format_status::ok) {
        return prefix_status;
    }
    if (prefix.flags != file_prefix_flag_identity ||
        prefix.payload_size != file_identity_size - file_prefix_size) {
        return format_status::corrupt;
    }

    file_identity identity = {};
    uint32_t checksum = 0;
    const bool decoded =
        read_u64_le(buf, buf_len, 28, &identity.database_id) &&
        read_u64_le(buf, buf_len, 36, &identity.file_size) &&
        read_u32_le(buf, buf_len, 44, &identity.slot) &&
        read_u32_le(buf, buf_len, file_identity_checksum_offset,
                    &checksum);
    if (!decoded) {
        return format_status::invalid_size;
    }

    identity.generation = prefix.generation;
    identity.kind = expected_kind;
    if (identity.database_id == 0 ||
        identity.file_size <= identity_region_size ||
        identity.file_size > max_fat32_file_size ||
        identity.file_size % format_alignment != 0 ||
        !is_zero_range(buf, 48, 4)) {
        return format_status::corrupt;
    }
    if (checksum != calc_record_crc(
            buf, file_identity_size,
            file_identity_checksum_offset)) {
        return format_status::corrupt;
    }

    *identity_out = identity;
    return format_status::ok;
}
