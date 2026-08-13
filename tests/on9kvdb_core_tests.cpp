#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "on9kvdb.hpp"

namespace
{
    int failures = 0;

    void check(bool condition, const char *message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", message);
            failures += 1;
        }
    }

    class memory_io final : public on9kvdb_io
    {
    public:
        memory_io() : bytes(CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE, 0) {}

        uint32_t block_size() const override
        {
            return 512;
        }

        uint64_t block_count() const override
        {
            return bytes.size() / block_size();
        }

        esp_err_t read_blocks(uint64_t start_block, void *destination, uint32_t count) override
        {
            const uint64_t offset = start_block * block_size();
            const uint64_t size = static_cast<uint64_t>(count) * block_size();
            if (destination == nullptr || offset > bytes.size() || size > bytes.size() - offset) {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(destination, bytes.data() + offset, static_cast<size_t>(size));
            return ESP_OK;
        }

        esp_err_t write_blocks(uint64_t start_block, const void *source, uint32_t count) override
        {
            const uint64_t offset = start_block * block_size();
            const uint64_t size = static_cast<uint64_t>(count) * block_size();
            if (source == nullptr || offset > bytes.size() || size > bytes.size() - offset) {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(bytes.data() + offset, source, static_cast<size_t>(size));
            return ESP_OK;
        }

        esp_err_t sync() override
        {
            if (fail_sync) {
                fail_sync = false;
                return ESP_ERR_INVALID_STATE;
            }
            return ESP_OK;
        }

        bool rewrite_value_chunk_payloads()
        {
            bool rewritten = false;
            for (size_t offset = 0; offset + on9kvdb_def::value_chunk_size <= bytes.size();
                 offset += on9kvdb_def::value_chunk_size) {
                on9kvdb_def::value_chunk_header header = {};
                const uint8_t *payload = nullptr;
                if (on9kvdb_def::decode_value_chunk(bytes.data() + offset, on9kvdb_def::value_chunk_size, &header, &payload) !=
                    on9kvdb_def::format_status::ok) {
                    continue;
                }
                std::array<uint8_t, on9kvdb_def::value_chunk_size> replacement = {};
                memcpy(replacement.data() + on9kvdb_def::value_chunk_header_size, payload, header.payload_size);
                replacement[on9kvdb_def::value_chunk_header_size] ^= UINT8_C(0x5a);
                if (!on9kvdb_def::encode_value_chunk(replacement.data(), replacement.size(), header,
                                                     replacement.data() + on9kvdb_def::value_chunk_header_size)) {
                    return false;
                }
                memcpy(bytes.data() + offset, replacement.data(), replacement.size());
                rewritten = true;
            }
            return rewritten;
        }

        std::vector<uint8_t> bytes;
        bool fail_sync = false;
    };

    on9kvdb_bytes as_bytes(const char *text)
    {
        return {reinterpret_cast<const uint8_t *>(text), static_cast<uint16_t>(strlen(text))};
    }

    esp_err_t read_exact(on9kvdb &database, on9kvdb_handle handle, const char *key, uint8_t *output, uint32_t size)
    {
        on9kvdb_value_reader reader = {};
        esp_err_t ret = database.open_value(handle, as_bytes(key), &reader);
        uint32_t read_size = 0;
        ret = ret ?: database.read_value_into(reader, output, size, &read_size);
        ret = ret ?: (read_size == size ? ESP_OK : ESP_ERR_INVALID_SIZE);
        const esp_err_t close_ret = reader.is_valid() ? database.close(reader) : ESP_OK;
        return ret ?: close_ret;
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

    void test_core_lifecycle_and_recovery()
    {
        memory_io storage;
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(&storage, &config);
        check(database.init() == ESP_OK, "initialize blank database");

        on9kvdb_handle handle = {};
        check(database.open(as_bytes("ns"), on9kvdb_open_mode::read_write, &handle) == ESP_OK, "open binary namespace");
        on9kvdb_transaction_handle transaction = {};
        check(database.begin(handle, &transaction) == ESP_OK, "begin transaction");
        const uint8_t initial[] = {1, 0, 2, 3};
        check(database.set(transaction, as_bytes("key"), initial, sizeof(initial)) == ESP_OK, "stage inline value");
        check(database.commit(transaction) == ESP_OK, "commit inline value");

        uint8_t recovered[sizeof(initial)] = {};
        check(read_exact(database, handle, "key", recovered, sizeof(recovered)) == ESP_OK &&
                  memcmp(initial, recovered, sizeof(initial)) == 0,
              "read committed inline value");
        check(database.close(handle) == ESP_OK, "close namespace before reboot");
        check(database.deinit() == ESP_OK, "deinitialize database");

        check(database.init() == ESP_OK, "recover database from WAL");
        check(database.open(as_bytes("ns"), on9kvdb_open_mode::read_write, &handle) == ESP_OK, "reopen recovered namespace");
        check(read_exact(database, handle, "key", recovered, sizeof(recovered)) == ESP_OK &&
                  memcmp(initial, recovered, sizeof(initial)) == 0,
              "read value after recovery");

        check(database.begin(handle, &transaction) == ESP_OK, "begin writer-lifetime transaction");
        on9kvdb_value_writer writer = {};
        check(database.begin_value_write(transaction, as_bytes("unfinished"), 2048, &writer) == ESP_OK,
              "begin progressive writer");
        check(database.commit(transaction) == ESP_ERR_INVALID_STATE, "reject commit with unfinished writer");
        check(database.abort(transaction) == ESP_ERR_INVALID_STATE, "reject abort with unfinished writer");
        check(database.abort_value_write(writer) == ESP_OK, "abort progressive writer explicitly");
        check(database.abort(transaction) == ESP_OK, "abort transaction after writer");

        std::vector<uint8_t> large(5000);
        for (size_t index = 0; index < large.size(); index += 1U) {
            large[index] = static_cast<uint8_t>(index * 17U);
        }
        check(database.begin(handle, &transaction) == ESP_OK, "begin external replacement transaction");
        check(database.set(transaction, as_bytes("replace"), large.data(), large.size()) == ESP_OK, "stage external value");
        const uint8_t replacement[] = {9, 8, 7};
        check(database.set(transaction, as_bytes("replace"), replacement, sizeof(replacement)) == ESP_OK,
              "replace staged external value with inline bytes");
        check(database.commit(transaction) == ESP_OK, "commit external-to-inline replacement");
        uint8_t replacement_read[sizeof(replacement)] = {};
        check(read_exact(database, handle, "replace", replacement_read, sizeof(replacement_read)) == ESP_OK &&
                  memcmp(replacement, replacement_read, sizeof(replacement)) == 0,
              "read replacement without stale external descriptor");

        check(database.begin(handle, &transaction) == ESP_OK, "begin external CRC transaction");
        check(database.set(transaction, as_bytes("large"), large.data(), large.size()) == ESP_OK, "stage CRC test value");
        check(database.commit(transaction) == ESP_OK, "commit CRC test value");
        check(storage.rewrite_value_chunk_payloads(), "rewrite valid chunks with different payload CRCs");
        std::vector<uint8_t> damaged(large.size());
        on9kvdb_value_reader reader = {};
        check(database.open_value(handle, as_bytes("large"), &reader) == ESP_OK, "open rewritten external value");
        uint32_t read_size = 0;
        check(database.read_value_into(reader, damaged.data(), damaged.size(), &read_size) == ESP_ERR_INVALID_CRC &&
                  read_size == damaged.size(),
              "whole-value checksum rejects reordered or independently re-encoded chunks");
        check(database.close(reader) == ESP_OK, "close CRC reader");

        check(database.close(handle) == ESP_OK, "close final namespace");
        check(database.deinit() == ESP_OK, "final deinitialize");
    }

    void test_table_accounting_and_staged_value_relocation()
    {
        memory_io storage;
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(&storage, &config);
        check(database.init() == ESP_OK, "initialize compaction database");
        on9kvdb_handle handle = {};
        check(database.open(as_bytes("compact"), on9kvdb_open_mode::read_write, &handle) == ESP_OK, "open compaction namespace");

        check(commit_batch(database, handle, 0, 7, 800) == ESP_OK, "commit first memtable batch");
        check(commit_batch(database, handle, 1, 7, 800) == ESP_OK, "flush first immutable table");
        check(commit_batch(database, handle, 2, 7, 800) == ESP_OK, "flush second immutable table");
        on9kvdb_stats stats = {};
        check(database.get_stats(&stats) == ESP_OK && stats.active_table_count == 2,
              "fill both active table slots before staged-value compaction");

        std::vector<uint8_t> large(5000);
        for (size_t index = 0; index < large.size(); index += 1U) {
            large[index] = static_cast<uint8_t>(index * 29U + 3U);
        }
        on9kvdb_transaction_handle transaction = {};
        check(database.begin(handle, &transaction) == ESP_OK, "begin relocation transaction");
        check(database.set(transaction, as_bytes("relocated"), large.data(), large.size()) == ESP_OK,
              "stage external value before bank switch");
        std::array<uint8_t, 900> inline_value = {};
        check(database.set(transaction, as_bytes("trigger-a"), inline_value.data(), inline_value.size()) == ESP_OK,
              "stage first compaction trigger");
        check(database.set(transaction, as_bytes("trigger-b"), inline_value.data(), inline_value.size()) == ESP_OK,
              "stage second compaction trigger");
        check(database.set(transaction, as_bytes("trigger-c"), inline_value.data(), inline_value.size()) == ESP_OK,
              "stage third compaction trigger");
        check(database.commit(transaction) == ESP_OK, "commit transaction across value-bank switching compaction");
        check(database.get_stats(&stats) == ESP_OK && stats.compaction_count > 0, "observe first compaction");
        const uint64_t first_compaction_count = stats.compaction_count;

        std::vector<uint8_t> readback(large.size());
        check(read_exact(database, handle, "relocated", readback.data(), readback.size()) == ESP_OK && readback == large,
              "read relocated staged value after first bank switch");

        for (uint32_t batch = 3; batch < 9 && stats.compaction_count == first_compaction_count; batch += 1U) {
            const esp_err_t batch_ret = commit_batch(database, handle, batch, 7, 800);
            check(batch_ret == ESP_OK, "commit batch toward second bank switch");
            check(database.get_stats(&stats) == ESP_OK, "read compaction statistics");
        }
        check(stats.compaction_count > first_compaction_count, "force a second value-bank switch");
        memset(readback.data(), 0, readback.size());
        check(read_exact(database, handle, "relocated", readback.data(), readback.size()) == ESP_OK && readback == large,
              "staged descriptor remains live after old bank is recycled");

        check(database.close(handle) == ESP_OK, "close compaction namespace before recovery");
        check(database.deinit() == ESP_OK, "deinitialize compacted database");
        check(database.init() == ESP_OK, "recover tables after compaction");
        check(database.open(as_bytes("compact"), on9kvdb_open_mode::read_write, &handle) == ESP_OK,
              "open compacted namespace after recovery");

        check(database.begin(handle, &transaction) == ESP_OK, "begin table-backed replacement");
        const uint8_t tiny[] = {4, 2};
        check(database.set(transaction, as_bytes("b00-k00"), tiny, sizeof(tiny)) == ESP_OK, "replace a table-backed value");
        check(database.commit(transaction) == ESP_OK, "commit table-backed replacement with stable logical accounting");

        check(database.begin(handle, &transaction) == ESP_OK, "begin retryable sync-failure transaction");
        check(database.set(transaction, as_bytes("sync-retry"), tiny, sizeof(tiny)) == ESP_OK, "stage sync retry value");
        storage.fail_sync = true;
        check(database.commit(transaction) != ESP_OK, "surface WAL sync failure");
        check(database.commit(transaction) == ESP_OK, "retry transaction after pre-publication sync failure");

        check(database.close(handle) == ESP_OK, "close recovered compaction namespace");
        check(database.deinit() == ESP_OK, "deinitialize accounting database");
    }

    void test_wal_rotation_rechecks_relocated_descriptor_checksum()
    {
        memory_io storage;
        const on9kvdb_cfg config = {CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET};
        on9kvdb database(&storage, &config);
        check(database.init() == ESP_OK, "initialize WAL rotation database");
        on9kvdb_handle handle = {};
        check(database.open(as_bytes("rotate"), on9kvdb_open_mode::read_write, &handle) == ESP_OK, "open WAL rotation namespace");

        // Fill both active table slots while leaving the current memtable
        // non-empty. Six one-frame replacements then consume the remaining WAL
        // capacity without increasing the memtable footprint.
        check(commit_batch(database, handle, 0, 7, 800) == ESP_OK, "commit first rotation batch");
        check(commit_batch(database, handle, 1, 7, 800) == ESP_OK, "commit second rotation batch");
        check(commit_batch(database, handle, 2, 7, 800) == ESP_OK, "commit third rotation batch");
        for (uint32_t index = 0; index < 6; index += 1U) {
            on9kvdb_transaction_handle transaction = {};
            const uint8_t value = static_cast<uint8_t>(index);
            esp_err_t ret = database.begin(handle, &transaction);
            ret = ret ?: database.set(transaction, as_bytes("replacement"), &value, sizeof(value));
            ret = ret ?: database.commit(transaction);
            check(ret == ESP_OK, "fill final WAL frames with replacements");
        }

        std::vector<uint8_t> external(5000, UINT8_C(0x5a));
        on9kvdb_transaction_handle transaction = {};
        esp_err_t ret = database.begin(handle, &transaction);
        ret = ret ?: database.set(transaction, as_bytes("external-after-rotation"), external.data(), external.size());
        ret = ret ?: database.commit(transaction);
        check(ret == ESP_OK, "commit external value across rotation-triggered compaction");
        check(database.close(handle) == ESP_OK, "close rotation namespace");
        check(database.deinit() == ESP_OK, "deinitialize rotation database");

        check(database.init() == ESP_OK, "recover rotation database");
        check(database.open(as_bytes("rotate"), on9kvdb_open_mode::read_write, &handle) == ESP_OK, "reopen rotation namespace");
        std::vector<uint8_t> recovered(external.size());
        check(read_exact(database, handle, "external-after-rotation", recovered.data(), recovered.size()) == ESP_OK &&
                  recovered == external,
              "recover external value committed after rotation-triggered relocation");
        check(database.close(handle) == ESP_OK, "close recovered rotation namespace");
        check(database.deinit() == ESP_OK, "deinitialize recovered rotation database");
    }
}

int main()
{
    test_core_lifecycle_and_recovery();
    test_table_accounting_and_staged_value_relocation();
    test_wal_rotation_rechecks_relocated_descriptor_checksum();
    return failures == 0 ? 0 : 1;
}
