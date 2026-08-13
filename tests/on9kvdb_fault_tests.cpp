#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "on9kvdb.hpp"

namespace
{
    static const constexpr uint32_t no_fault = UINT32_MAX;
    int failures = 0;

    void check(bool condition, const char *message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", message);
            failures += 1;
        }
    }

    class crash_io final : public on9kvdb_io
    {
    public:
        crash_io() : working(CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE, 0), durable(working) {}

        uint32_t block_size() const override
        {
            return 512;
        }

        uint64_t block_count() const override
        {
            return working.size() / block_size();
        }

        esp_err_t read_blocks(uint64_t start_block, void *destination, uint32_t count) override
        {
            const uint64_t offset = start_block * block_size();
            const uint64_t size = static_cast<uint64_t>(count) * block_size();
            if (destination == nullptr || offset > working.size() || size > working.size() - offset) {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(destination, working.data() + offset, static_cast<size_t>(size));
            return ESP_OK;
        }

        esp_err_t write_blocks(uint64_t start_block, const void *source, uint32_t count) override
        {
            const uint64_t offset = start_block * block_size();
            const uint64_t size = static_cast<uint64_t>(count) * block_size();
            const uint32_t call = write_calls;
            write_calls += 1U;
            if (source == nullptr || offset > working.size() || size > working.size() - offset) {
                return ESP_ERR_INVALID_ARG;
            }
            if (call == fail_write_call) {
                return ESP_ERR_INVALID_STATE;
            }
            memcpy(working.data() + offset, source, static_cast<size_t>(size));
            return ESP_OK;
        }

        esp_err_t sync() override
        {
            const uint32_t call = sync_calls;
            sync_calls += 1U;
            if (call == fail_sync_call) {
                return ESP_ERR_INVALID_STATE;
            }
            durable = working;
            return ESP_OK;
        }

        void arm(uint32_t write_call, uint32_t sync_call)
        {
            write_calls = 0;
            sync_calls = 0;
            fail_write_call = write_call;
            fail_sync_call = sync_call;
        }

        void crash()
        {
            working = durable;
            arm(no_fault, no_fault);
        }

        bool corrupt_newest_manifest_copy()
        {
            uint64_t greatest_generation = 0;
            size_t selected_offset = 0;
            for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1U) {
                const size_t offset = static_cast<size_t>(slot) * on9kvdb_def::manifest_slot_size;
                on9kvdb_def::manifest_record record = {};
                if (on9kvdb_def::decode_manifest_record(durable.data() + offset, on9kvdb_def::manifest_record_size, &record) ==
                        on9kvdb_def::format_status::ok &&
                    record.generation > greatest_generation) {
                    greatest_generation = record.generation;
                    selected_offset = offset;
                }
            }
            if (greatest_generation == 0) {
                return false;
            }
            durable[selected_offset] ^= UINT8_C(0x80);
            working = durable;
            return true;
        }

        std::vector<uint8_t> working;
        std::vector<uint8_t> durable;
        uint32_t write_calls = 0;
        uint32_t sync_calls = 0;
        uint32_t fail_write_call = no_fault;
        uint32_t fail_sync_call = no_fault;
    };

    enum class fault_scenario : uint8_t {
        external_commit,
        table_flush,
        full_compaction,
    };

    struct operation_result {
        esp_err_t ret = ESP_ERR_INVALID_STATE;
        uint32_t write_calls = 0;
        uint32_t sync_calls = 0;
    };

    on9kvdb_bytes as_bytes(const char *text)
    {
        return {reinterpret_cast<const uint8_t *>(text), static_cast<uint16_t>(strlen(text))};
    }

    esp_err_t commit_batch(on9kvdb &database, on9kvdb_handle handle, uint32_t batch, uint32_t count, uint32_t value_size)
    {
        std::vector<uint8_t> value(value_size, static_cast<uint8_t>(batch + 1U));
        on9kvdb_transaction_handle transaction = {};
        esp_err_t ret = database.begin(handle, &transaction);
        for (uint32_t index = 0; ret == ESP_OK && index < count; index += 1U) {
            char key[24] = {};
            std::snprintf(key, sizeof(key), "b%02u-k%02u", static_cast<unsigned>(batch), static_cast<unsigned>(index));
            ret = database.set(transaction, as_bytes(key), value.data(), value.size());
        }
        return ret ?: database.commit(transaction);
    }

    bool key_exists(on9kvdb &database, on9kvdb_handle handle, const char *key)
    {
        on9kvdb_value_reader reader = {};
        const esp_err_t ret = database.open_value(handle, as_bytes(key), &reader);
        if (ret == ESP_OK) {
            (void)database.close(reader);
            return true;
        }
        return false;
    }

    const char *candidate_key(fault_scenario scenario)
    {
        if (scenario == fault_scenario::external_commit) {
            return "external-candidate";
        }
        return scenario == fault_scenario::table_flush ? "b01-k00" : "b03-k00";
    }

    bool prepare_scenario(crash_io *storage, fault_scenario scenario)
    {
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(storage, &config);
        on9kvdb_handle handle = {};
        esp_err_t ret = database.init();
        ret = ret ?: database.open(as_bytes("faults"), on9kvdb_open_mode::read_write, &handle);
        if (scenario == fault_scenario::external_commit) {
            ret = ret ?: commit_batch(database, handle, 0, 1, 32);
        } else {
            ret = ret ?: commit_batch(database, handle, 0, 7, 800);
            if (scenario == fault_scenario::full_compaction) {
                ret = ret ?: commit_batch(database, handle, 1, 7, 800);
                ret = ret ?: commit_batch(database, handle, 2, 7, 800);
            }
        }
        ret = ret ?: database.close(handle);
        ret = ret ?: database.deinit();
        storage->crash();
        return ret == ESP_OK;
    }

    operation_result run_operation(crash_io *storage, fault_scenario scenario, uint32_t fail_write_call, uint32_t fail_sync_call)
    {
        operation_result result = {};
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(storage, &config);
        on9kvdb_handle handle = {};
        result.ret = database.init();
        result.ret = result.ret ?: database.open(as_bytes("faults"), on9kvdb_open_mode::read_write, &handle);
        if (result.ret != ESP_OK) {
            return result;
        }

        storage->arm(fail_write_call, fail_sync_call);
        if (scenario == fault_scenario::external_commit) {
            std::vector<uint8_t> value(5000, UINT8_C(0x6d));
            on9kvdb_transaction_handle transaction = {};
            result.ret = database.begin(handle, &transaction);
            result.ret = result.ret ?: database.set(transaction, as_bytes(candidate_key(scenario)), value.data(), value.size());
            result.ret = result.ret ?: database.commit(transaction);
        } else if (scenario == fault_scenario::table_flush) {
            result.ret = commit_batch(database, handle, 1, 7, 800);
        } else {
            result.ret = commit_batch(database, handle, 3, 7, 800);
        }
        result.write_calls = storage->write_calls;
        result.sync_calls = storage->sync_calls;
        (void)database.deinit(true);
        return result;
    }

    bool recover_expected_state(crash_io *storage, fault_scenario scenario, bool candidate_expected)
    {
        storage->crash();
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(storage, &config);
        on9kvdb_handle handle = {};
        esp_err_t ret = database.init();
        ret = ret ?: database.open(as_bytes("faults"), on9kvdb_open_mode::read_write, &handle);
        if (ret != ESP_OK) {
            return false;
        }
        const bool baseline_exists = key_exists(database, handle, "b00-k00");
        const bool candidate_exists = key_exists(database, handle, candidate_key(scenario));
        (void)database.close(handle);
        (void)database.deinit();
        return baseline_exists && candidate_exists == candidate_expected;
    }

    void test_fault_matrix(fault_scenario scenario, const char *message)
    {
        crash_io baseline;
        check(prepare_scenario(&baseline, scenario), "prepare durable fault-injection scenario");

        crash_io probe = baseline;
        const operation_result successful = run_operation(&probe, scenario, no_fault, no_fault);
        check(successful.ret == ESP_OK && successful.write_calls > 0 && successful.sync_calls > 0,
              "measure successful publication boundaries");
        check(recover_expected_state(&probe, scenario, true), "recover successful operation from durable image");

        bool matrix_ok = true;
        for (uint32_t call = 0; call < successful.write_calls; call += 1U) {
            crash_io trial = baseline;
            const operation_result result = run_operation(&trial, scenario, call, no_fault);
            if (result.ret == ESP_OK || !recover_expected_state(&trial, scenario, false)) {
                matrix_ok = false;
                break;
            }
        }
        for (uint32_t call = 0; matrix_ok && call < successful.sync_calls; call += 1U) {
            crash_io trial = baseline;
            const operation_result result = run_operation(&trial, scenario, no_fault, call);
            if (result.ret == ESP_OK || !recover_expected_state(&trial, scenario, false)) {
                matrix_ok = false;
                break;
            }
        }
        check(matrix_ok, message);
    }

    void test_ready_manifest_is_redundant_before_first_commit()
    {
        crash_io storage;
        check(prepare_scenario(&storage, fault_scenario::external_commit), "prepare newly provisioned ready database");
        check(storage.corrupt_newest_manifest_copy(), "corrupt newest ready manifest copy");
        check(recover_expected_state(&storage, fault_scenario::external_commit, false),
              "fallback ready manifest preserves first-run acknowledged WAL data");
    }
}

int main()
{
    test_fault_matrix(fault_scenario::external_commit, "external commit survives every write and sync failure boundary");
    test_fault_matrix(fault_scenario::table_flush, "table flush survives every write and sync failure boundary");
    test_fault_matrix(fault_scenario::full_compaction, "full compaction survives every write and sync failure boundary");
    test_ready_manifest_is_redundant_before_first_commit();
    return failures == 0 ? 0 : 1;
}
