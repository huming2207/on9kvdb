#pragma once

#include <cstddef>
#include <cstdint>

#define ON9KVDB_PACKED __attribute__((packed, aligned(1)))

namespace on9kvdb_def
{
    static const constexpr size_t max_name_len = 32;
    static const constexpr size_t max_value_len = 8192;
    static const constexpr size_t max_transaction_mutations = 10;
    static const constexpr size_t runtime_memory_budget_default =
        100U * 1024U;
    static const constexpr size_t runtime_memory_budget_max =
        200U * 1024U - 1U;
    static const constexpr uint64_t max_fat32_file_size = UINT32_MAX;

    static const constexpr uint16_t storage_revision = 1;
    static const constexpr uint32_t manifest_magic = 0x394d564bUL; // "KVM9"
    static const constexpr uint32_t wal_magic = 0x3957564bUL; // "KVW9"
    static const constexpr uint32_t table_magic = 0x3954564bUL; // "KVT9"

    enum class file_kind : uint16_t {
        invalid = 0,
        manifest = 1,
        wal = 2,
        table = 3,
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
    static const constexpr size_t file_prefix_checksum_offset =
        offsetof(file_prefix, checksum);

    struct decoded_file_prefix {
        uint32_t magic = 0;
        uint16_t revision = 0;
        file_kind kind = file_kind::invalid;
        uint16_t flags = 0;
        uint64_t generation = 0;
        uint32_t payload_size = 0;
        uint32_t checksum = 0;
    };

    bool checked_add_size(size_t lhs, size_t rhs, size_t *result_out);
    bool checked_mul_size(size_t lhs, size_t rhs, size_t *result_out);
    bool checked_align_up_size(size_t value, size_t alignment,
                               size_t *result_out);
    bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out);
    bool checked_mul_u64(uint64_t lhs, uint64_t rhs, uint64_t *result_out);

    bool write_u16_le(uint8_t *buf, size_t buf_len, size_t offset,
                      uint16_t value);
    bool write_u32_le(uint8_t *buf, size_t buf_len, size_t offset,
                      uint32_t value);
    bool write_u64_le(uint8_t *buf, size_t buf_len, size_t offset,
                      uint64_t value);
    bool read_u16_le(const uint8_t *buf, size_t buf_len, size_t offset,
                     uint16_t *value_out);
    bool read_u32_le(const uint8_t *buf, size_t buf_len, size_t offset,
                     uint32_t *value_out);
    bool read_u64_le(const uint8_t *buf, size_t buf_len, size_t offset,
                     uint64_t *value_out);

    uint32_t calc_crc32(const uint8_t *buf, size_t len);
    uint32_t calc_crc32_update(uint32_t crc, const uint8_t *buf, size_t len);

    bool validate_name(const char *name, size_t *length_out = nullptr);

    uint32_t make_handle_value(uint16_t slot, uint16_t generation);
    bool decode_handle_value(uint32_t value, uint16_t *slot_out,
                             uint16_t *generation_out);
    bool is_handle_value(uint32_t value, uint16_t slot,
                         uint16_t generation);

    bool encode_file_prefix(uint8_t *buf, size_t buf_len, uint32_t magic,
                            file_kind kind, uint16_t flags,
                            uint64_t generation, uint32_t payload_size);
    format_status decode_file_prefix(const uint8_t *buf, size_t buf_len,
                                     uint32_t expected_magic,
                                     file_kind expected_kind,
                                     decoded_file_prefix *prefix_out);
}
