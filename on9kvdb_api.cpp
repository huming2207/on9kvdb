#include <inttypes.h>
#include <cstring>

#include <esp_log.h>

#include "on9kvdb.hpp"

esp_err_t on9kvdb::get_handle_slot_unsafe(on9kvdb_handle handle, handle_slot **slot_out, uint16_t *slot_index_out)
{
    const handle_slot *slot = nullptr;
    const esp_err_t ret = static_cast<const on9kvdb *>(this)->get_handle_slot_unsafe(handle, &slot, slot_index_out);
    if (ret == ESP_OK) {
        *slot_out = const_cast<handle_slot *>(slot);
    }
    return ret;
}

esp_err_t on9kvdb::get_handle_slot_unsafe(on9kvdb_handle handle, const handle_slot **slot_out, uint16_t *slot_index_out) const
{
    if (slot_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t slot_index = 0;
    uint32_t generation = 0;
    if (!on9kvdb_def::decode_handle_value(handle.raw, &slot_index, &generation) ||
        slot_index >= CONFIG_ON9KVDB_MAX_OPEN_HANDLES) {
        return ESP_ERR_INVALID_ARG;
    }

    const handle_slot &slot = handles[slot_index];
    if (!slot.used || slot.generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }

    *slot_out = &slot;
    if (slot_index_out != nullptr) {
        *slot_index_out = slot_index;
    }
    return ESP_OK;
}

esp_err_t on9kvdb::get_transaction_unsafe(on9kvdb_transaction_handle transaction_handle, transaction_slot **slot_out)
{
    const transaction_slot *slot = nullptr;
    const esp_err_t ret = static_cast<const on9kvdb *>(this)->get_transaction_unsafe(transaction_handle, &slot);
    if (ret == ESP_OK) {
        *slot_out = const_cast<transaction_slot *>(slot);
    }
    return ret;
}

esp_err_t on9kvdb::get_transaction_unsafe(on9kvdb_transaction_handle transaction_handle, const transaction_slot **slot_out) const
{
    if (slot_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t slot_index = 0;
    uint32_t generation = 0;
    if (!on9kvdb_def::decode_handle_value(transaction_handle.raw, &slot_index, &generation) || slot_index != 0 ||
        transaction == nullptr || !transaction->active || transaction->generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }

    if (transaction->handle_slot_index >= CONFIG_ON9KVDB_MAX_OPEN_HANDLES) {
        return ESP_ERR_INVALID_STATE;
    }
    const handle_slot &handle = handles[transaction->handle_slot_index];
    if (!handle.used || handle.generation != transaction->handle_generation || !handle.transaction_active) {
        return ESP_ERR_INVALID_STATE;
    }

    *slot_out = transaction;
    return ESP_OK;
}

void on9kvdb::clear_transaction_unsafe()
{
    if (transaction == nullptr) {
        return;
    }

    const uint32_t generation = transaction->generation;
    if (transaction->active && transaction->handle_slot_index < CONFIG_ON9KVDB_MAX_OPEN_HANDLES) {
        handle_slot &handle = handles[transaction->handle_slot_index];
        if (handle.used && handle.generation == transaction->handle_generation) {
            handle.transaction_active = false;
        }
    }

    *transaction = {};
    transaction->generation = generation;
}

esp_err_t on9kvdb::open(on9kvdb_bytes namespace_name, on9kvdb_open_mode mode, on9kvdb_handle *handle_out)
{
    if (handle_out == nullptr || (mode != on9kvdb_open_mode::read_only && mode != on9kvdb_open_mode::read_write)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!on9kvdb_def::validate_bytes(namespace_name.data, namespace_name.size)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t namespace_index = 0;
    ret = find_namespace_unsafe(namespace_name, &namespace_index);
    if (ret == ESP_ERR_NOT_FOUND && mode == on9kvdb_open_mode::read_write) {
        ret = ensure_namespace_capacity_unsafe(namespace_name, &namespace_index, false);
    }
    if (ret != ESP_OK) {
        release_operation_lock();
        return ret;
    }

    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_OPEN_HANDLES; idx += 1) {
        handle_slot &slot = handles[idx];
        if (slot.used) {
            continue;
        }

        if (handle_generation_counter == 0) {
            release_operation_lock();
            return ESP_ERR_INVALID_STATE;
        }
        const uint32_t generation = handle_generation_counter;
        handle_generation_counter = generation == on9kvdb_def::max_handle_generation ? 0 : handle_generation_counter + 1U;
        slot = {};
        slot.generation = generation;
        slot.used = true;
        slot.mode = mode;
        slot.namespace_size = namespace_name.size;
        memcpy(slot.namespace_name, namespace_name.data, namespace_name.size);
        *handle_out = on9kvdb_handle(on9kvdb_def::make_handle_value(static_cast<uint16_t>(idx), generation));
        release_operation_lock();
        return ESP_OK;
    }

    release_operation_lock();
    return ESP_ERR_NO_MEM;
}

esp_err_t on9kvdb::close(on9kvdb_handle handle)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    handle_slot *slot = nullptr;
    ret = get_handle_slot_unsafe(handle, &slot);
    if (ret == ESP_OK && slot->transaction_active) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        const uint32_t generation = slot->generation;
        *slot = {};
        slot->generation = generation;
    }

    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::begin(on9kvdb_handle handle, on9kvdb_transaction_handle *transaction_out)
{
    if (transaction_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    handle_slot *handle_state = nullptr;
    uint16_t handle_index = 0;
    ret = get_handle_slot_unsafe(handle, &handle_state, &handle_index);
    if (ret == ESP_OK && handle_state->mode != on9kvdb_open_mode::read_write) {
        ret = ESP_ERR_NOT_ALLOWED;
    }
    if (ret == ESP_OK && (transaction->active || handle_state->transaction_active)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        if (transaction_generation_counter == 0) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            const uint32_t generation = transaction_generation_counter;
            transaction_generation_counter =
                generation == on9kvdb_def::max_handle_generation ? 0 : transaction_generation_counter + 1U;
            *transaction = {};
            transaction->generation = generation;
            transaction->handle_slot_index = handle_index;
            transaction->handle_generation = handle_state->generation;
            transaction->active = true;
            handle_state->transaction_active = true;
            *transaction_out = on9kvdb_transaction_handle(on9kvdb_def::make_handle_value(0, generation));
        }
    }

    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::stage_value_unsafe(on9kvdb_transaction_handle transaction_handle, on9kvdb_bytes key, const void *value,
                                      size_t value_size, uint8_t mutation_kind)
{
    if (!on9kvdb_def::validate_bytes(key.data, key.size) || value_size > on9kvdb_def::inline_value_len ||
        (value == nullptr && value_size != 0) ||
        (mutation_kind != on9kvdb_def::mutation_kind_set && mutation_kind != on9kvdb_def::mutation_kind_tombstone)) {
        return ESP_ERR_INVALID_ARG;
    }

    transaction_slot *transaction_state = nullptr;
    esp_err_t ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret != ESP_OK) {
        return ret;
    }

    mutation_slot *mutation = find_staged_mutation_unsafe(transaction_state, key);
    const uint32_t old_value_size = mutation == nullptr ? 0 : mutation->value_size;
    const uint32_t new_staged_size = transaction_state->staged_value_bytes - old_value_size + static_cast<uint32_t>(value_size);
    if (new_staged_size > CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mutation == nullptr && transaction_state->mutation_count >= CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (mutation != nullptr && old_value_size > 0) {
        const uint32_t tail_offset = mutation->value_offset + old_value_size;
        const uint32_t tail_size = transaction_state->staged_value_bytes - tail_offset;
        if (tail_size > 0) {
            memmove(transaction_staging + mutation->value_offset, transaction_staging + tail_offset, tail_size);
        }
        for (uint16_t idx = 0; idx < transaction_state->mutation_count; idx += 1) {
            mutation_slot &candidate = transaction_state->mutations[idx];
            if (&candidate != mutation && candidate.value_size > 0 && candidate.value_offset > mutation->value_offset) {
                candidate.value_offset -= old_value_size;
            }
        }
        transaction_state->staged_value_bytes -= old_value_size;
    }

    if (mutation == nullptr) {
        mutation = &transaction_state->mutations[transaction_state->mutation_count];
        *mutation = {};
        transaction_state->mutation_count += 1;
    }

    mutation->value_offset = transaction_state->staged_value_bytes;
    mutation->value_size = static_cast<uint32_t>(value_size);
    mutation->key_size = key.size;
    mutation->reserved0 = 0;
    mutation->kind = mutation_kind;
    memcpy(mutation->key, key.data, key.size);
    if (value_size > 0) {
        memcpy(transaction_staging + transaction_state->staged_value_bytes, value, value_size);
    }
    transaction_state->staged_value_bytes = new_staged_size;
    return ESP_OK;
}

esp_err_t on9kvdb::stage_external_value_unsafe(on9kvdb_transaction_handle transaction_handle, on9kvdb_bytes key,
                                               const on9kvdb_def::value_ref &reference)
{
    if (!on9kvdb_def::value_ref_is_valid(reference, manifest.geometry.value_bank_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = stage_value_unsafe(transaction_handle, key, nullptr, 0, on9kvdb_def::mutation_kind_set);
    if (ret != ESP_OK) {
        return ret;
    }
    transaction_slot *transaction_state = nullptr;
    ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret != ESP_OK) {
        return ret;
    }
    mutation_slot *mutation = find_staged_mutation_unsafe(transaction_state, key);
    if (mutation == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    mutation->value_size = reference.value_size;
    mutation->external_value = true;
    mutation->external_value_ref = reference;
    return ESP_OK;
}

esp_err_t on9kvdb::erase_key(on9kvdb_transaction_handle transaction_handle, on9kvdb_bytes key)
{
    if (!on9kvdb_def::validate_bytes(key.data, key.size)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    transaction_slot *transaction_state = nullptr;
    ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret == ESP_OK) {
        const handle_slot &handle = handles[transaction_state->handle_slot_index];
        value_view current = {};
        ret = lookup_transaction_unsafe(*transaction_state, handle, key, &current);
        if (ret == ESP_OK && current.tombstone) {
            ret = ESP_ERR_NOT_FOUND;
        }
    }
    if (ret == ESP_OK) {
        ret = stage_value_unsafe(transaction_handle, key, nullptr, 0, on9kvdb_def::mutation_kind_tombstone);
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::set(on9kvdb_transaction_handle transaction_handle, on9kvdb_bytes key, const uint8_t *value,
                       uint32_t value_size)
{
    if (!on9kvdb_def::validate_bytes(key.data, key.size) || (value == nullptr && value_size != 0) ||
        value_size > on9kvdb_def::max_value_len) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (value_size <= on9kvdb_def::inline_value_len) {
        ret = stage_value_unsafe(transaction_handle, key, value, value_size, on9kvdb_def::mutation_kind_set);
    } else {
        on9kvdb_value_writer writer = {};
        ret = begin_value_write_unsafe(transaction_handle, key, value_size, &writer);
        ret = ret ?: write_value_unsafe(value_writer, value, value_size);
        ret = ret ?: finish_value_write_unsafe(value_writer);
        if (ret != ESP_OK && value_writer != nullptr && value_writer->active) {
            const uint32_t generation = value_writer->generation;
            *value_writer = {};
            value_writer->generation = generation;
        }
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::commit(on9kvdb_transaction_handle transaction_handle)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }

    transaction_slot *transaction_state = nullptr;
    ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret != ESP_OK) {
        release_operation_lock();
        return ret;
    }
    if (transaction_state->mutation_count == 0) {
        clear_transaction_unsafe();
        release_operation_lock();
        return ESP_OK;
    }

    handle_slot &handle = handles[transaction_state->handle_slot_index];
    compact_memtable_unsafe();
    uint16_t namespace_index = 0;
    const on9kvdb_bytes namespace_name = {handle.namespace_name, handle.namespace_size};
    ret = preflight_memtable_transaction_unsafe(*transaction_state, namespace_name, &namespace_index);
    if (ret == ESP_ERR_NO_MEM && memtable_entry_count > 0) {
        ret = flush_memtable_unsafe();
        if (ret == ESP_OK) {
            ret = preflight_memtable_transaction_unsafe(*transaction_state, namespace_name, &namespace_index);
        }
    }
    bool wal_durable = false;
    if (ret == ESP_OK && value_bank_dirty_mask != 0) {
        // A WAL descriptor can make a large value reachable after reboot.  Persist its chunks before the WAL commit record
        // is written, otherwise recovery could publish a reference to unwritten media.
        ret = sync_storage_unsafe();
        if (ret == ESP_OK) {
            value_bank_dirty_mask = 0;
        }
    }
    if (ret == ESP_OK) {
        ret = append_transaction_unsafe(transaction_state, handle);
        wal_durable = ret == ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = ensure_namespace_capacity_unsafe(namespace_name, &namespace_index, true);
    }
    if (ret == ESP_OK) {
        ret = apply_transaction_to_memtable_unsafe(*transaction_state, namespace_index, next_transaction_sequence);
        if (ret == ESP_OK) {
            next_transaction_sequence += 1U;
            clear_transaction_unsafe();
        }
    }
    if (wal_durable && ret != ESP_OK) {
        // The transaction is already durable, so retrying it with the same sequence would create a duplicate WAL record.
        // Latch the fault and require recovery to publish the durable transaction into a fresh memtable.
        ESP_LOGE(TAG, "Commit: post-WAL publication invariant failed: ret=%s, sequence=%" PRIu64, esp_err_to_name(ret),
                 next_transaction_sequence);
        storage_faulted = true;
        clear_transaction_unsafe();
    }

    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::close(on9kvdb_transaction_handle transaction_handle)
{
    return commit(transaction_handle);
}

esp_err_t on9kvdb::abort(on9kvdb_transaction_handle transaction_handle)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    transaction_slot *transaction_state = nullptr;
    ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret == ESP_OK) {
        clear_transaction_unsafe();
    }
    release_operation_lock();
    return ret;
}

#if 0 // Removed vNext typed/string API; retained temporarily as migration reference only.
#define ON9KVDB_DEFINE_FIXED_GETTER(method_name, value_type, kv_type)                                                            \
    esp_err_t on9kvdb::method_name(on9kvdb_handle handle, const char *key, value_type *value_out) const                          \
    {                                                                                                                            \
        on9kvdb_bytes key_bytes = {};                                                                                            \
        if (!make_cstring_bytes(key, &key_bytes)) {                                                                              \
            return ESP_ERR_INVALID_ARG;                                                                                          \
        }                                                                                                                        \
        esp_err_t ret = acquire_operation_lock();                                                                                \
        if (ret != ESP_OK) {                                                                                                     \
            return ret;                                                                                                          \
        }                                                                                                                        \
        const handle_slot *handle_state = nullptr;                                                                               \
        ret = get_handle_slot_unsafe(handle, &handle_state);                                                                     \
        value_view view = {};                                                                                                    \
        if (ret == ESP_OK) {                                                                                                     \
            ret = lookup_committed_unsafe({handle_state->namespace_name, handle_state->namespace_size}, key_bytes, &view);       \
        }                                                                                                                        \
        if (ret == ESP_OK) {                                                                                                     \
            ret = get_fixed_value_unsafe(view, kv_type, value_out, sizeof(*value_out));                                          \
        }                                                                                                                        \
        release_operation_lock();                                                                                                \
        return ret;                                                                                                              \
    }                                                                                                                            \
                                                                                                                                 \
    esp_err_t on9kvdb::method_name(on9kvdb_transaction_handle transaction_handle, const char *key, value_type *value_out) const  \
    {                                                                                                                            \
        on9kvdb_bytes key_bytes = {};                                                                                            \
        if (!make_cstring_bytes(key, &key_bytes)) {                                                                              \
            return ESP_ERR_INVALID_ARG;                                                                                          \
        }                                                                                                                        \
        esp_err_t ret = acquire_operation_lock();                                                                                \
        if (ret != ESP_OK) {                                                                                                     \
            return ret;                                                                                                          \
        }                                                                                                                        \
        const transaction_slot *transaction_state = nullptr;                                                                     \
        ret = get_transaction_unsafe(transaction_handle, &transaction_state);                                                    \
        value_view view = {};                                                                                                    \
        if (ret == ESP_OK) {                                                                                                     \
            const handle_slot &handle_state = handles[transaction_state->handle_slot_index];                                     \
            ret = lookup_transaction_unsafe(*transaction_state, handle_state, key_bytes, &view);                                 \
        }                                                                                                                        \
        if (ret == ESP_OK) {                                                                                                     \
            ret = get_fixed_value_unsafe(view, kv_type, value_out, sizeof(*value_out));                                          \
        }                                                                                                                        \
        release_operation_lock();                                                                                                \
        return ret;                                                                                                              \
    }

ON9KVDB_DEFINE_FIXED_GETTER(get_i8, int8_t, on9kvdb_type::i8)
ON9KVDB_DEFINE_FIXED_GETTER(get_u8, uint8_t, on9kvdb_type::u8)
ON9KVDB_DEFINE_FIXED_GETTER(get_i16, int16_t, on9kvdb_type::i16)
ON9KVDB_DEFINE_FIXED_GETTER(get_u16, uint16_t, on9kvdb_type::u16)
ON9KVDB_DEFINE_FIXED_GETTER(get_i32, int32_t, on9kvdb_type::i32)
ON9KVDB_DEFINE_FIXED_GETTER(get_u32, uint32_t, on9kvdb_type::u32)
ON9KVDB_DEFINE_FIXED_GETTER(get_i64, int64_t, on9kvdb_type::i64)
ON9KVDB_DEFINE_FIXED_GETTER(get_u64, uint64_t, on9kvdb_type::u64)

#undef ON9KVDB_DEFINE_FIXED_GETTER

#define ON9KVDB_DEFINE_VARIABLE_GETTER(method_name, output_type, kv_type)                                                        \
    esp_err_t on9kvdb::method_name(on9kvdb_handle handle, const char *key, output_type value_out, size_t *length) const          \
    {                                                                                                                            \
        on9kvdb_bytes key_bytes = {};                                                                                            \
        if (!make_cstring_bytes(key, &key_bytes)) {                                                                              \
            return ESP_ERR_INVALID_ARG;                                                                                          \
        }                                                                                                                        \
        esp_err_t ret = acquire_operation_lock();                                                                                \
        if (ret != ESP_OK) {                                                                                                     \
            return ret;                                                                                                          \
        }                                                                                                                        \
        const handle_slot *handle_state = nullptr;                                                                               \
        ret = get_handle_slot_unsafe(handle, &handle_state);                                                                     \
        value_view view = {};                                                                                                    \
        if (ret == ESP_OK) {                                                                                                     \
            ret = lookup_committed_unsafe({handle_state->namespace_name, handle_state->namespace_size}, key_bytes, &view);       \
        }                                                                                                                        \
        if (ret == ESP_OK) {                                                                                                     \
            ret = get_variable_value_unsafe(view, kv_type, value_out, length);                                                   \
        }                                                                                                                        \
        release_operation_lock();                                                                                                \
        return ret;                                                                                                              \
    }                                                                                                                            \
                                                                                                                                 \
    esp_err_t on9kvdb::method_name(on9kvdb_transaction_handle transaction_handle, const char *key, output_type value_out,        \
                                   size_t *length) const                                                                         \
    {                                                                                                                            \
        on9kvdb_bytes key_bytes = {};                                                                                            \
        if (!make_cstring_bytes(key, &key_bytes)) {                                                                              \
            return ESP_ERR_INVALID_ARG;                                                                                          \
        }                                                                                                                        \
        esp_err_t ret = acquire_operation_lock();                                                                                \
        if (ret != ESP_OK) {                                                                                                     \
            return ret;                                                                                                          \
        }                                                                                                                        \
        const transaction_slot *transaction_state = nullptr;                                                                     \
        ret = get_transaction_unsafe(transaction_handle, &transaction_state);                                                    \
        value_view view = {};                                                                                                    \
        if (ret == ESP_OK) {                                                                                                     \
            const handle_slot &handle_state = handles[transaction_state->handle_slot_index];                                     \
            ret = lookup_transaction_unsafe(*transaction_state, handle_state, key_bytes, &view);                                 \
        }                                                                                                                        \
        if (ret == ESP_OK) {                                                                                                     \
            ret = get_variable_value_unsafe(view, kv_type, value_out, length);                                                   \
        }                                                                                                                        \
        release_operation_lock();                                                                                                \
        return ret;                                                                                                              \
    }

ON9KVDB_DEFINE_VARIABLE_GETTER(get_str, char *, on9kvdb_type::str)
ON9KVDB_DEFINE_VARIABLE_GETTER(get_blob, void *, on9kvdb_type::blob)

#undef ON9KVDB_DEFINE_VARIABLE_GETTER

esp_err_t on9kvdb::find_key(on9kvdb_handle handle, const char *key, on9kvdb_type *type_out) const
{
    if (type_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    on9kvdb_bytes key_bytes = {};
    if (!make_cstring_bytes(key, &key_bytes)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    const handle_slot *handle_state = nullptr;
    ret = get_handle_slot_unsafe(handle, &handle_state);
    value_view view = {};
    if (ret == ESP_OK) {
        ret = lookup_committed_unsafe({handle_state->namespace_name, handle_state->namespace_size}, key_bytes, &view);
    }
    if (ret == ESP_OK && view.tombstone) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        *type_out = view.type;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::find_key(on9kvdb_transaction_handle transaction_handle, const char *key, on9kvdb_type *type_out) const
{
    if (type_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    on9kvdb_bytes key_bytes = {};
    if (!make_cstring_bytes(key, &key_bytes)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    const transaction_slot *transaction_state = nullptr;
    ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    value_view view = {};
    if (ret == ESP_OK) {
        const handle_slot &handle = handles[transaction_state->handle_slot_index];
        ret = lookup_transaction_unsafe(*transaction_state, handle, key_bytes, &view);
    }
    if (ret == ESP_OK && view.tombstone) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        *type_out = view.type;
    }
    release_operation_lock();
    return ret;
}

#endif

esp_err_t on9kvdb::get_stats(on9kvdb_stats *stats_out) const
{
    if (stats_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = acquire_diagnostic_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    *stats_out = stats;
    stats_out->logical_state_capacity_bytes = manifest.geometry.max_live_bytes;
    stats_out->provisioned_database_bytes = manifest.geometry.provisioned_size;
    stats_out->runtime_memory_bytes = cfg.runtime_memory_budget;
    stats_out->runtime_scratch_bytes = future_scratch_size;
    stats_out->manifest_generation = manifest.generation;
    stats_out->safe_checkpoint_sequence = manifest.safe_checkpoint_sequence;
    stats_out->valid_manifest_copy_count = manifest_valid_copy_count;
    stats_out->active_table_bank = manifest.active_table_bank;
    stats_out->active_wal_slot = manifest.active_wal_slot;
    stats_out->manifest_stabilization_required = manifest_stabilization_required;
    stats_out->storage_faulted = storage_faulted;
    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1) {
        if (manifest.tables[slot].active) {
            stats_out->active_table_count += 1U;
        }
    }
    for (uint32_t slot = 0; slot < on9kvdb_def::wal_file_count; slot += 1) {
        if (manifest.wal_generation[slot] != 0) {
            stats_out->referenced_wal_count += 1U;
            stats_out->wal_record_capacity_bytes += manifest.geometry.wal_size - on9kvdb_def::wal_record_region_offset;
        }
    }
    release_operation_lock();
    return ESP_OK;
}
