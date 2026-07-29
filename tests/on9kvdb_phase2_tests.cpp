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

    static const constexpr uint32_t table_count = 6;
    static const constexpr uint32_t data_file_count = on9kvdb_def::wal_file_count + table_count;

    on9kvdb_def::storage_geometry make_geometry()
    {
        on9kvdb_def::storage_geometry geometry = {};
        geometry.provisioned_size = 4U * 1024U * 1024U;
        geometry.max_live_bytes = 2U * 1024U * 1024U;
        geometry.manifest_size = on9kvdb_def::manifest_file_size;
        geometry.wal_size = 256U * 1024U;
        geometry.wal_count = on9kvdb_def::wal_file_count;
        geometry.table_size = 596U * 1024U;
        geometry.table_count = table_count;
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
        limits.max_transaction_mutations = on9kvdb_def::max_transaction_mutations;
        limits.transaction_staging_bytes = 24U * 1024U;
        limits.sstable_block_bytes = 12U * 1024U;
        return limits;
    }

    bool select_manifest(const uint8_t slots[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size],
                         on9kvdb_def::manifest_record *selected_out, uint32_t *valid_count_out)
    {
        bool found = false;
        on9kvdb_def::manifest_record selected = {};
        uint32_t valid_count = 0;
        for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
            on9kvdb_def::manifest_record candidate = {};
            const on9kvdb_def::format_status status =
                on9kvdb_def::decode_manifest_record(slots[slot], on9kvdb_def::manifest_record_size, &candidate);
            if (status != on9kvdb_def::format_status::ok) {
                continue;
            }

            valid_count += 1;
            if (!found || candidate.generation > selected.generation) {
                selected = candidate;
                found = true;
            }
        }

        if (selected_out != nullptr && found) {
            *selected_out = selected;
        }
        if (valid_count_out != nullptr) {
            *valid_count_out = valid_count;
        }
        return found;
    }

    struct model_file {
        bool exists = false;
        uint8_t working[on9kvdb_def::identity_slot_count][on9kvdb_def::file_identity_size] = {};
        uint8_t durable[on9kvdb_def::identity_slot_count][on9kvdb_def::file_identity_size] = {};
    };

    struct model_image {
        bool manifest_exists = false;
        uint8_t manifest_working[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size] = {};
        uint8_t manifest_durable[on9kvdb_def::manifest_slot_count][on9kvdb_def::manifest_record_size] = {};
        model_file files[data_file_count] = {};

        void reset()
        {
            memcpy(manifest_working, manifest_durable, sizeof(manifest_working));
            for (uint32_t idx = 0; idx < data_file_count; idx += 1) {
                memcpy(files[idx].working, files[idx].durable, sizeof(files[idx].working));
            }
        }
    };

    struct event_injector {
        int stop_after = -1;
        int event_count = 0;

        bool interrupted()
        {
            const bool stop = event_count == stop_after;
            event_count += 1;
            return stop;
        }
    };

    enum class model_result : uint8_t {
        complete,
        interrupted,
        fail_closed,
    };

    bool write_manifest_event(model_image *image, const on9kvdb_def::manifest_record &record, event_injector *injector)
    {
        const uint32_t slot = static_cast<uint32_t>((record.generation - 1U) % on9kvdb_def::manifest_slot_count);
        EXPECT_TRUE(
            on9kvdb_def::encode_manifest_record(image->manifest_working[slot], on9kvdb_def::manifest_record_size, record));
        return injector->interrupted();
    }

    bool sync_manifest_event(model_image *image, event_injector *injector)
    {
        memcpy(image->manifest_durable, image->manifest_working, sizeof(image->manifest_durable));
        return injector->interrupted();
    }

    bool write_identity_event(model_image *image, uint32_t file_index, uint32_t copy_slot,
                              const on9kvdb_def::file_identity &identity, event_injector *injector)
    {
        EXPECT_TRUE(on9kvdb_def::encode_file_identity(image->files[file_index].working[copy_slot],
                                                      on9kvdb_def::file_identity_size, identity));
        return injector->interrupted();
    }

    bool sync_identity_event(model_image *image, uint32_t file_index, event_injector *injector)
    {
        memcpy(image->files[file_index].durable, image->files[file_index].working, sizeof(image->files[file_index].durable));
        return injector->interrupted();
    }

    model_result run_provisioning(model_image *image, event_injector *injector)
    {
        const on9kvdb_def::storage_geometry geometry = make_geometry();
        static const constexpr uint64_t database_id = UINT64_C(0x1122334455667788);
        on9kvdb_def::manifest_record state = {};
        uint32_t valid_manifest_copies = 0;

        if (!image->manifest_exists) {
            image->manifest_exists = true;
            state.database_id = database_id;
            state.geometry = geometry;
            state.limits = make_limits();
            state.state = on9kvdb_def::manifest_state_provisioning_owned;
            state.generation = 1;
            if (write_manifest_event(image, state, injector)) {
                return model_result::interrupted;
            }
            if (sync_manifest_event(image, injector)) {
                return model_result::interrupted;
            }

            state.generation = 2;
            if (write_manifest_event(image, state, injector)) {
                return model_result::interrupted;
            }
            if (sync_manifest_event(image, injector)) {
                return model_result::interrupted;
            }
            valid_manifest_copies = on9kvdb_def::manifest_slot_count;
        } else if (!select_manifest(image->manifest_working, &state, &valid_manifest_copies)) {
            return model_result::fail_closed;
        }

        if (state.state == on9kvdb_def::manifest_state_ready) {
            return model_result::complete;
        }
        if (state.state != on9kvdb_def::manifest_state_provisioning_owned) {
            return model_result::fail_closed;
        }

        if (valid_manifest_copies < on9kvdb_def::manifest_slot_count) {
            state.generation += 1;
            if (write_manifest_event(image, state, injector)) {
                return model_result::interrupted;
            }
            if (sync_manifest_event(image, injector)) {
                return model_result::interrupted;
            }
        }

        for (uint32_t file_index = 0; file_index < data_file_count; file_index += 1) {
            model_file &file = image->files[file_index];
            file.exists = true;
            const bool wal = file_index < on9kvdb_def::wal_file_count;
            const on9kvdb_def::file_kind kind = wal ? on9kvdb_def::file_kind::wal : on9kvdb_def::file_kind::table;
            const uint32_t slot = wal ? file_index : file_index - on9kvdb_def::wal_file_count;
            const uint64_t file_size = wal ? geometry.wal_size : geometry.table_size;

            for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::identity_slot_count; copy_slot += 1) {
                on9kvdb_def::file_identity decoded = {};
                const on9kvdb_def::format_status status =
                    on9kvdb_def::decode_file_identity(file.working[copy_slot], on9kvdb_def::file_identity_size, kind, &decoded);
                if (status == on9kvdb_def::format_status::ok) {
                    const bool matches = decoded.database_id == database_id && decoded.file_size == file_size &&
                                         decoded.slot == slot && decoded.generation == 1;
                    if (!matches) {
                        return model_result::fail_closed;
                    }
                    continue;
                }

                on9kvdb_def::file_identity identity = {};
                identity.generation = 1;
                identity.database_id = database_id;
                identity.file_size = file_size;
                identity.slot = slot;
                identity.kind = kind;
                if (write_identity_event(image, file_index, copy_slot, identity, injector)) {
                    return model_result::interrupted;
                }
                if (sync_identity_event(image, file_index, injector)) {
                    return model_result::interrupted;
                }
            }
        }

        state.generation += 1;
        state.state = on9kvdb_def::manifest_state_ready;
        state.active_wal_slot = 0;
        state.wal_generation[0] = 1;
        if (write_manifest_event(image, state, injector)) {
            return model_result::interrupted;
        }
        if (sync_manifest_event(image, injector)) {
            return model_result::interrupted;
        }
        return model_result::complete;
    }

    void test_geometry_and_records()
    {
        const on9kvdb_def::storage_geometry geometry = make_geometry();
        EXPECT_TRUE(on9kvdb_def::validate_storage_geometry(geometry));

        on9kvdb_def::storage_geometry mismatch = geometry;
        mismatch.table_count = 3;
        mismatch.max_live_bytes = 1024U * 1024U;
        mismatch.provisioned_size = mismatch.manifest_size + static_cast<uint64_t>(mismatch.wal_count) * mismatch.wal_size +
                                    static_cast<uint64_t>(mismatch.table_count) * mismatch.table_size;
        EXPECT_TRUE(!on9kvdb_def::storage_geometry_equal(geometry, mismatch));
        EXPECT_TRUE(on9kvdb_def::validate_storage_geometry(mismatch));

        on9kvdb_def::manifest_record record = {};
        record.generation = 7;
        record.database_id = UINT64_C(0x0102030405060708);
        record.state = on9kvdb_def::manifest_state_ready;
        record.geometry = geometry;
        record.limits = make_limits();
        record.active_wal_slot = 0;
        record.wal_generation[0] = 1;

        uint8_t encoded[on9kvdb_def::manifest_record_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_manifest_record(encoded, sizeof(encoded), record));
        on9kvdb_def::manifest_record decoded = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok, on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded));
        EXPECT_EQ(record.generation, decoded.generation);
        EXPECT_TRUE(on9kvdb_def::storage_geometry_equal(record.geometry, decoded.geometry));
        EXPECT_TRUE(on9kvdb_def::logical_limits_equal(record.limits, decoded.limits));

        encoded[64] ^= 1U;
        EXPECT_EQ(on9kvdb_def::format_status::corrupt, on9kvdb_def::decode_manifest_record(encoded, sizeof(encoded), &decoded));

        on9kvdb_def::file_identity identity = {};
        identity.generation = 1;
        identity.database_id = record.database_id;
        identity.file_size = geometry.table_size;
        identity.slot = 5;
        identity.kind = on9kvdb_def::file_kind::table;
        uint8_t identity_bytes[on9kvdb_def::file_identity_size] = {};
        EXPECT_TRUE(on9kvdb_def::encode_file_identity(identity_bytes, sizeof(identity_bytes), identity));
        on9kvdb_def::file_identity decoded_identity = {};
        EXPECT_EQ(on9kvdb_def::format_status::ok,
                  on9kvdb_def::decode_file_identity(identity_bytes, sizeof(identity_bytes), on9kvdb_def::file_kind::table,
                                                    &decoded_identity));
        EXPECT_EQ(identity.slot, decoded_identity.slot);
    }

    void test_every_provisioning_write_and_sync()
    {
        model_image baseline = {};
        event_injector baseline_injector = {};
        EXPECT_EQ(model_result::complete, run_provisioning(&baseline, &baseline_injector));
        const int event_count = baseline_injector.event_count;
        EXPECT_TRUE(event_count > 0);

        for (int stop_after = 0; stop_after < event_count; stop_after += 1) {
            model_image image = {};
            event_injector first = {};
            first.stop_after = stop_after;
            EXPECT_EQ(model_result::interrupted, run_provisioning(&image, &first));
            image.reset();

            event_injector recovery = {};
            const model_result recovered = run_provisioning(&image, &recovery);
            if (stop_after == 0) {
                EXPECT_EQ(model_result::fail_closed, recovered);
                continue;
            }

            EXPECT_EQ(model_result::complete, recovered);
            on9kvdb_def::manifest_record selected = {};
            EXPECT_TRUE(select_manifest(image.manifest_durable, &selected, nullptr));
            EXPECT_EQ(on9kvdb_def::manifest_state_ready, selected.state);
        }
    }
}

int main()
{
    test_geometry_and_records();
    test_every_provisioning_write_and_sync();

    if (failure_count != 0) {
        std::fprintf(stderr, "%d Phase 2 test(s) failed\n", failure_count);
        return 1;
    }

    std::puts("on9kvdb Phase 2 tests passed");
    return 0;
}
