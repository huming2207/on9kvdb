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

    static const constexpr uint64_t database_id = UINT64_C(0x0123456789abcdef);
    static const constexpr uint64_t wal_generation = 7;
    static const constexpr uint64_t transaction_sequence = 19;

    void fill_payload(uint8_t *payload, size_t size)
    {
        for (size_t idx = 0; idx < size; idx += 1) {
            payload[idx] = static_cast<uint8_t>(((idx * 37U) % 251U) + 1U);
        }
    }

    on9kvdb_def::wal_frame_header make_frame_header(uint16_t frame_index, uint16_t frame_count, uint32_t payload_size,
                                                    uint32_t transaction_size, uint32_t transaction_checksum)
    {
        on9kvdb_def::wal_frame_header header = {};
        header.database_id = database_id;
        header.wal_generation = wal_generation;
        header.transaction_sequence = transaction_sequence;
        header.frame_index = frame_index;
        header.frame_count = frame_count;
        header.mutation_count = 2;
        header.flags = frame_index + 1U == frame_count ? on9kvdb_def::wal_frame_flag_commit : 0;
        header.payload_size = payload_size;
        header.transaction_payload_size = transaction_size;
        header.transaction_checksum = transaction_checksum;
        return header;
    }

    bool transaction_is_committed(const uint8_t *frames, uint16_t frame_count)
    {
        uint32_t checksum = UINT32_MAX;
        uint32_t payload_size = 0;
        on9kvdb_def::wal_frame_header first = {};
        for (uint16_t idx = 0; idx < frame_count; idx += 1) {
            on9kvdb_def::wal_frame_header header = {};
            const uint8_t *payload = nullptr;
            if (on9kvdb_def::decode_wal_frame(frames + static_cast<size_t>(idx) * on9kvdb_def::wal_frame_size,
                                              on9kvdb_def::wal_frame_size, &header, &payload) != on9kvdb_def::format_status::ok) {
                return false;
            }

            if (idx == 0) {
                first = header;
            } else if (header.database_id != first.database_id || header.wal_generation != first.wal_generation ||
                       header.transaction_sequence != first.transaction_sequence || header.frame_count != first.frame_count ||
                       header.mutation_count != first.mutation_count ||
                       header.transaction_payload_size != first.transaction_payload_size ||
                       header.transaction_checksum != first.transaction_checksum) {
                return false;
            }
            if (header.frame_index != idx || header.frame_count != frame_count ||
                header.payload_size > first.transaction_payload_size - payload_size) {
                return false;
            }

            checksum = on9kvdb_def::calc_crc32_update(checksum, payload, header.payload_size);
            payload_size += header.payload_size;
        }

        return payload_size == first.transaction_payload_size && ~checksum == first.transaction_checksum;
    }

    void test_wal_header_round_trip_and_corruption()
    {
        on9kvdb_def::wal_header header = {};
        header.database_id = database_id;
        header.generation = wal_generation;
        header.first_transaction_sequence = transaction_sequence;
        header.slot = 1;
        header.record_region_start = on9kvdb_def::wal_record_region_offset;
        header.record_region_end = 256U * 1024U;
        header.frame_size = on9kvdb_def::wal_frame_size;
        header.state = on9kvdb_def::wal_header_state_active;

        uint8_t encoded[on9kvdb_def::wal_header_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_wal_header(encoded, sizeof(encoded), header));

        on9kvdb_def::wal_header decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok, on9kvdb_def::decode_wal_header(encoded, sizeof(encoded), &decoded));
        EXPECT_TRUE(on9kvdb_def::wal_header_equal(header, decoded));

        encoded[4] = 0xff;
        encoded[5] = 0xff;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt, on9kvdb_def::decode_wal_header(encoded, sizeof(encoded), &decoded));
        encoded[4] = static_cast<uint8_t>(on9kvdb_def::wal_header_revision);
        encoded[5] = 0;

        for (size_t idx = 0; idx < sizeof(encoded); idx += 1) {
            encoded[idx] ^= 1U;
            EXPECT_TRUE(on9kvdb_def::decode_wal_header(encoded, sizeof(encoded), &decoded) != on9kvdb_def::format_status::ok);
            encoded[idx] ^= 1U;
        }
    }

    void test_full_frame_detects_every_torn_prefix_and_bit_flip()
    {
        uint8_t payload[on9kvdb_def::wal_frame_payload_capacity] = {};
        fill_payload(payload, sizeof(payload));
        const uint32_t checksum = on9kvdb_def::calc_crc32(payload, sizeof(payload));
        const on9kvdb_def::wal_frame_header header = make_frame_header(0, 1, sizeof(payload), sizeof(payload), checksum);

        uint8_t valid[on9kvdb_def::wal_frame_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_wal_frame(valid, sizeof(valid), header, payload, sizeof(payload)));

        on9kvdb_def::wal_frame_header decoded = {};
        const uint8_t *decoded_payload = nullptr;
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_wal_frame(valid, sizeof(valid), &decoded, &decoded_payload));
        EXPECT_EQ(0, memcmp(payload, decoded_payload, sizeof(payload)));

        valid[4] = 0xff;
        valid[5] = 0xff;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt,
                  on9kvdb_def::decode_wal_frame(valid, sizeof(valid), &decoded, &decoded_payload));
        valid[4] = static_cast<uint8_t>(on9kvdb_def::wal_frame_revision);
        valid[5] = 0;

        uint8_t interrupted[on9kvdb_def::wal_frame_size] = {};
        for (size_t cut = 0; cut < sizeof(valid); cut += 1) {
            memset(interrupted, 0xff, sizeof(interrupted));
            memcpy(interrupted, valid, cut);
            EXPECT_TRUE(on9kvdb_def::decode_wal_frame(interrupted, sizeof(interrupted), &decoded, &decoded_payload) !=
                        on9kvdb_def::format_status::ok);
        }

        for (size_t idx = 0; idx < sizeof(valid); idx += 1) {
            valid[idx] ^= 1U;
            EXPECT_TRUE(on9kvdb_def::decode_wal_frame(valid, sizeof(valid), &decoded, &decoded_payload) !=
                        on9kvdb_def::format_status::ok);
            valid[idx] ^= 1U;
        }
    }

    void test_two_frame_transaction_is_all_or_nothing()
    {
        static const constexpr uint32_t transaction_size = on9kvdb_def::wal_frame_payload_capacity + 777U;
        uint8_t payload[transaction_size] = {};
        fill_payload(payload, sizeof(payload));
        const uint32_t checksum = on9kvdb_def::calc_crc32(payload, sizeof(payload));

        uint8_t valid[2U * on9kvdb_def::wal_frame_size] = {};
        for (uint16_t idx = 0; idx < 2; idx += 1) {
            const uint32_t payload_offset = static_cast<uint32_t>(idx) * on9kvdb_def::wal_frame_payload_capacity;
            const uint32_t part_size = transaction_size - payload_offset < on9kvdb_def::wal_frame_payload_capacity
                                           ? transaction_size - payload_offset
                                           : on9kvdb_def::wal_frame_payload_capacity;
            const on9kvdb_def::wal_frame_header header = make_frame_header(idx, 2, part_size, transaction_size, checksum);
            EXPECT_TRUE(on9kvdb_def::encode_wal_frame(valid + static_cast<size_t>(idx) * on9kvdb_def::wal_frame_size,
                                                      on9kvdb_def::wal_frame_size, header, payload + payload_offset, part_size));
        }
        EXPECT_TRUE(transaction_is_committed(valid, 2));

        uint8_t interrupted[2U * on9kvdb_def::wal_frame_size] = {};
        for (size_t cut = 0; cut < sizeof(valid); cut += 1) {
            memset(interrupted, 0xff, sizeof(interrupted));
            memcpy(interrupted, valid, cut);
            EXPECT_TRUE(!transaction_is_committed(interrupted, 2));
        }

        memcpy(interrupted, valid, sizeof(valid));
        interrupted[on9kvdb_def::wal_frame_size + 100U] ^= 1U;
        EXPECT_TRUE(!transaction_is_committed(interrupted, 2));
    }

    void test_write_sync_publish_boundaries()
    {
        static const constexpr uint32_t transaction_size = on9kvdb_def::wal_frame_payload_capacity + 97U;
        uint8_t payload[transaction_size] = {};
        fill_payload(payload, sizeof(payload));
        const uint32_t checksum = on9kvdb_def::calc_crc32(payload, sizeof(payload));
        uint8_t valid[2U * on9kvdb_def::wal_frame_size] = {};
        for (uint16_t idx = 0; idx < 2; idx += 1) {
            const uint32_t payload_offset = static_cast<uint32_t>(idx) * on9kvdb_def::wal_frame_payload_capacity;
            const uint32_t part_size = transaction_size - payload_offset < on9kvdb_def::wal_frame_payload_capacity
                                           ? transaction_size - payload_offset
                                           : on9kvdb_def::wal_frame_payload_capacity;
            const on9kvdb_def::wal_frame_header header = make_frame_header(idx, 2, part_size, transaction_size, checksum);
            EXPECT_TRUE(on9kvdb_def::encode_wal_frame(valid + static_cast<size_t>(idx) * on9kvdb_def::wal_frame_size,
                                                      on9kvdb_def::wal_frame_size, header, payload + payload_offset, part_size));
        }

        for (uint32_t stop_after = 0; stop_after < 5; stop_after += 1) {
            uint8_t working[2U * on9kvdb_def::wal_frame_size] = {};
            uint8_t durable[2U * on9kvdb_def::wal_frame_size] = {};
            memset(working, 0xff, sizeof(working));
            memset(durable, 0xff, sizeof(durable));
            bool visible = false;
            bool acknowledged = false;

            memcpy(working, valid, on9kvdb_def::wal_frame_size);
            if (stop_after >= 1) {
                memcpy(working + on9kvdb_def::wal_frame_size, valid + on9kvdb_def::wal_frame_size, on9kvdb_def::wal_frame_size);
            }
            if (stop_after >= 2) {
                memcpy(durable, working, sizeof(durable));
            }
            if (stop_after >= 3) {
                visible = true;
            }
            if (stop_after >= 4) {
                acknowledged = true;
            }

            visible = transaction_is_committed(durable, 2);
            EXPECT_EQ(stop_after >= 2, visible);
            if (acknowledged) {
                EXPECT_TRUE(visible);
            }
        }
    }
}

int main()
{
    test_wal_header_round_trip_and_corruption();
    test_full_frame_detects_every_torn_prefix_and_bit_flip();
    test_two_frame_transaction_is_all_or_nothing();
    test_write_sync_publish_boundaries();

    if (failure_count != 0) {
        std::fprintf(stderr, "%d Phase 3 test(s) failed\n", failure_count);
        return 1;
    }

    std::puts("on9kvdb Phase 3 tests passed");
    return 0;
}
