#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

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

    struct model_entry {
        char key[on9kvdb_def::max_name_len + 1] = {};
        uint32_t value = 0;
        bool used = false;
        bool tombstone = false;
    };

    class fixed_model
    {
    public:
        bool stage_set(const char *key, uint32_t value)
        {
            if (!on9kvdb_def::validate_name(key)) {
                return false;
            }

            model_entry *entry = find_or_create(staged, key);
            if (entry == nullptr) {
                return false;
            }
            entry->value = value;
            entry->tombstone = false;
            return true;
        }

        bool stage_erase(const char *key)
        {
            if (!on9kvdb_def::validate_name(key)) {
                return false;
            }

            model_entry *entry = find_or_create(staged, key);
            if (entry == nullptr) {
                return false;
            }
            entry->tombstone = true;
            return true;
        }

        bool get_committed(const char *key, uint32_t *value_out) const
        {
            return get_from(committed, key, value_out);
        }

        bool get_transaction(const char *key, uint32_t *value_out) const
        {
            if (value_out == nullptr) {
                return false;
            }

            const model_entry *entry = find(staged, key);
            if (entry != nullptr) {
                if (entry->tombstone) {
                    return false;
                }
                *value_out = entry->value;
                return true;
            }

            return get_committed(key, value_out);
        }

        bool commit()
        {
            model_entry next[entry_capacity] = {};
            for (size_t idx = 0; idx < entry_capacity; idx += 1) {
                next[idx] = committed[idx];
            }

            for (size_t idx = 0; idx < entry_capacity; idx += 1) {
                if (!staged[idx].used) {
                    continue;
                }

                model_entry *destination = find_or_create(next, staged[idx].key);
                if (destination == nullptr) {
                    return false;
                }
                destination->value = staged[idx].value;
                destination->tombstone = staged[idx].tombstone;
            }

            for (size_t idx = 0; idx < entry_capacity; idx += 1) {
                committed[idx] = next[idx];
                staged[idx] = {};
            }
            return true;
        }

    private:
        static const constexpr size_t entry_capacity = 16;

        static const model_entry *find(const model_entry *entries, const char *key)
        {
            for (size_t idx = 0; idx < entry_capacity; idx += 1) {
                if (entries[idx].used && strcmp(entries[idx].key, key) == 0) {
                    return &entries[idx];
                }
            }
            return nullptr;
        }

        static model_entry *find_or_create(model_entry *entries, const char *key)
        {
            model_entry *free_entry = nullptr;
            for (size_t idx = 0; idx < entry_capacity; idx += 1) {
                if (entries[idx].used && strcmp(entries[idx].key, key) == 0) {
                    return &entries[idx];
                }
                if (!entries[idx].used && free_entry == nullptr) {
                    free_entry = &entries[idx];
                }
            }

            if (free_entry == nullptr) {
                return nullptr;
            }
            const size_t len = strlen(key);
            memcpy(free_entry->key, key, len + 1);
            free_entry->used = true;
            return free_entry;
        }

        static bool get_from(const model_entry *entries, const char *key, uint32_t *value_out)
        {
            if (value_out == nullptr) {
                return false;
            }

            const model_entry *entry = find(entries, key);
            if (entry == nullptr || entry->tombstone) {
                return false;
            }
            *value_out = entry->value;
            return true;
        }

    private:
        model_entry committed[entry_capacity] = {};
        model_entry staged[entry_capacity] = {};
    };

    void test_checked_arithmetic()
    {
        size_t size_result = 0;
        EXPECT_TRUE(on9kvdb_def::checked_add_size(2, 3, &size_result));
        EXPECT_EQ(static_cast<size_t>(5), size_result);
        EXPECT_TRUE(!on9kvdb_def::checked_add_size(std::numeric_limits<size_t>::max(), 1, &size_result));

        EXPECT_TRUE(on9kvdb_def::checked_mul_size(7, 9, &size_result));
        EXPECT_EQ(static_cast<size_t>(63), size_result);
        EXPECT_TRUE(!on9kvdb_def::checked_mul_size(std::numeric_limits<size_t>::max(), 2, &size_result));

        EXPECT_TRUE(on9kvdb_def::checked_align_up_size(17, 8, &size_result));
        EXPECT_EQ(static_cast<size_t>(24), size_result);
        EXPECT_TRUE(!on9kvdb_def::checked_align_up_size(17, 3, &size_result));

        uint64_t u64_result = 0;
        EXPECT_TRUE(on9kvdb_def::checked_add_u64(10, 20, &u64_result));
        EXPECT_EQ(UINT64_C(30), u64_result);
        EXPECT_TRUE(!on9kvdb_def::checked_add_u64(UINT64_MAX, 1, &u64_result));
        EXPECT_TRUE(!on9kvdb_def::checked_mul_u64(UINT64_MAX, 2, &u64_result));
    }

    void test_name_validation()
    {
        size_t len = 0;
        EXPECT_TRUE(on9kvdb_def::validate_name("a", &len));
        EXPECT_EQ(static_cast<size_t>(1), len);

        char maximum[on9kvdb_def::max_name_len + 1] = {};
        memset(maximum, 'x', on9kvdb_def::max_name_len);
        EXPECT_TRUE(on9kvdb_def::validate_name(maximum, &len));
        EXPECT_EQ(on9kvdb_def::max_name_len, len);

        char excessive[on9kvdb_def::max_name_len + 2] = {};
        memset(excessive, 'x', on9kvdb_def::max_name_len + 1);
        EXPECT_TRUE(!on9kvdb_def::validate_name(excessive));
        EXPECT_TRUE(!on9kvdb_def::validate_name(""));
        EXPECT_TRUE(!on9kvdb_def::validate_name(nullptr));
    }

    void test_endian_codec()
    {
        uint8_t encoded[14] = {};
        EXPECT_TRUE(on9kvdb_def::write_u16_le(encoded, sizeof(encoded), 0, 0x1234));
        EXPECT_TRUE(on9kvdb_def::write_u32_le(encoded, sizeof(encoded), 2, 0x89abcdefUL));
        EXPECT_TRUE(on9kvdb_def::write_u64_le(encoded, sizeof(encoded), 6, UINT64_C(0x0123456789abcdef)));

        static const constexpr uint8_t expected[] = {
            0x34, 0x12, 0xef, 0xcd, 0xab, 0x89, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
        };
        EXPECT_EQ(0, memcmp(encoded, expected, sizeof(expected)));

        uint16_t value16 = 0;
        uint32_t value32 = 0;
        uint64_t value64 = 0;
        EXPECT_TRUE(on9kvdb_def::read_u16_le(encoded, sizeof(encoded), 0, &value16));
        EXPECT_TRUE(on9kvdb_def::read_u32_le(encoded, sizeof(encoded), 2, &value32));
        EXPECT_TRUE(on9kvdb_def::read_u64_le(encoded, sizeof(encoded), 6, &value64));
        EXPECT_EQ(static_cast<uint16_t>(0x1234), value16);
        EXPECT_EQ(UINT32_C(0x89abcdef), value32);
        EXPECT_EQ(UINT64_C(0x0123456789abcdef), value64);
        EXPECT_TRUE(!on9kvdb_def::write_u64_le(encoded, sizeof(encoded), 7, value64));
    }

    void test_crc()
    {
        static const constexpr uint8_t input[] = "123456789";
        EXPECT_EQ(UINT32_C(0xcbf43926), on9kvdb_def::calc_crc32(input, sizeof(input) - 1));
        EXPECT_EQ(UINT32_C(0), on9kvdb_def::calc_crc32(nullptr, 0));
    }

    void test_handle_codec()
    {
        const uint32_t value = on9kvdb_def::make_handle_value(7, 9);
        EXPECT_TRUE(value != 0);

        uint16_t slot = 0;
        uint16_t generation = 0;
        EXPECT_TRUE(on9kvdb_def::decode_handle_value(value, &slot, &generation));
        EXPECT_EQ(static_cast<uint16_t>(7), slot);
        EXPECT_EQ(static_cast<uint16_t>(9), generation);
        EXPECT_TRUE(on9kvdb_def::is_handle_value(value, 7, 9));
        EXPECT_TRUE(!on9kvdb_def::is_handle_value(value, 7, 10));
        EXPECT_EQ(UINT32_C(0), on9kvdb_def::make_handle_value(UINT16_MAX, 1));
        EXPECT_EQ(UINT32_C(0), on9kvdb_def::make_handle_value(0, 0));
        EXPECT_TRUE(!on9kvdb_def::decode_handle_value(0, &slot, &generation));
    }

    void test_file_prefix()
    {
        uint8_t encoded[on9kvdb_def::file_prefix_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::wal_magic, on9kvdb_def::file_kind::wal,
                                                    0x1234, UINT64_C(0x0102030405060708), UINT32_C(0x11223344)));

        static const constexpr uint8_t expected[] = {
            0x4b, 0x56, 0x57, 0x39, 0x03, 0x00, 0x1c, 0x00, 0x02, 0x00, 0x34, 0x12, 0x08, 0x07,
            0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11, 0xc1, 0x2d, 0xca, 0xb7,
        };
        EXPECT_EQ(0, memcmp(encoded, expected, sizeof(expected)));

        on9kvdb_def::decoded_file_prefix decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::wal_magic, on9kvdb_def::file_kind::wal,
                                                  &decoded));
        EXPECT_EQ(UINT64_C(0x0102030405060708), decoded.generation);
        EXPECT_EQ(UINT32_C(0x11223344), decoded.payload_size);
        EXPECT_EQ(static_cast<uint16_t>(0x1234), decoded.flags);

        encoded[20] ^= 0x01;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt,
                  on9kvdb_def::decode_file_prefix(encoded, sizeof(encoded), on9kvdb_def::wal_magic, on9kvdb_def::file_kind::wal,
                                                  &decoded));
    }

    void test_visibility_model()
    {
        fixed_model model;
        uint32_t value = 0;

        EXPECT_TRUE(model.stage_set("key", 10));
        EXPECT_TRUE(!model.get_committed("key", &value));
        EXPECT_TRUE(model.get_transaction("key", &value));
        EXPECT_EQ(UINT32_C(10), value);
        EXPECT_TRUE(model.commit());
        EXPECT_TRUE(model.get_committed("key", &value));
        EXPECT_EQ(UINT32_C(10), value);

        EXPECT_TRUE(model.stage_set("key", 11));
        EXPECT_TRUE(model.get_committed("key", &value));
        EXPECT_EQ(UINT32_C(10), value);
        EXPECT_TRUE(model.get_transaction("key", &value));
        EXPECT_EQ(UINT32_C(11), value);

        EXPECT_TRUE(model.stage_erase("key"));
        EXPECT_TRUE(!model.get_transaction("key", &value));
        EXPECT_TRUE(model.get_committed("key", &value));
        EXPECT_TRUE(model.commit());
        EXPECT_TRUE(!model.get_committed("key", &value));
    }
}

int main()
{
    test_checked_arithmetic();
    test_name_validation();
    test_endian_codec();
    test_crc();
    test_handle_codec();
    test_file_prefix();
    test_visibility_model();

    if (failure_count != 0) {
        std::fprintf(stderr, "%d Phase 1 test(s) failed\n", failure_count);
        return 1;
    }

    std::puts("on9kvdb Phase 1 tests passed");
    return 0;
}
