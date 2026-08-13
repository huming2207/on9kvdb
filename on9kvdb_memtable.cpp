#include <cstring>

#include "on9kvdb.hpp"

namespace
{
    static const constexpr uint8_t previous_state_none = 0;
    static const constexpr uint8_t previous_state_live = 1;
    static const constexpr uint8_t previous_state_tombstone = 2;

    uint32_t align_record_size(uint32_t size)
    {
        return (size + 7U) & ~UINT32_C(7);
    }

    uint64_t logical_record_size(uint16_t namespace_size, uint16_t key_size, uint32_t value_size)
    {
        // Logical capacity describes the persistent KV state, not the larger
        // implementation-only memtable header.  Using the SSTable record
        // header here keeps commit, recovery, and compaction accounting equal.
        const uint64_t size =
            static_cast<uint64_t>(on9kvdb_def::table_entry_header_size) + namespace_size + key_size + value_size;
        return (size + 7U) & ~UINT64_C(7);
    }
}

esp_err_t on9kvdb::find_namespace_unsafe(on9kvdb_bytes namespace_name, uint16_t *slot_index_out) const
{
    if (!on9kvdb_def::validate_bytes(namespace_name.data, namespace_name.size) || slot_index_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_NAMESPACES; idx += 1) {
        if (namespaces[idx].used && namespaces[idx].name_size == namespace_name.size &&
            memcmp(namespaces[idx].name, namespace_name.data, namespace_name.size) == 0) {
            *slot_index_out = static_cast<uint16_t>(idx);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t on9kvdb::ensure_namespace_capacity_unsafe(on9kvdb_bytes namespace_name, uint16_t *slot_index_out, bool publish)
{
    if (!on9kvdb_def::validate_bytes(namespace_name.data, namespace_name.size) || slot_index_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = find_namespace_unsafe(namespace_name, slot_index_out);
    if (ret == ESP_OK) {
        return ESP_OK;
    }
    if (ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }
    if (namespace_count >= CONFIG_ON9KVDB_MAX_NAMESPACES) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_NAMESPACES; idx += 1) {
        if (namespaces[idx].used) {
            continue;
        }

        *slot_index_out = static_cast<uint16_t>(idx);
        if (publish) {
            namespace_slot &slot = namespaces[idx];
            slot = {};
            slot.used = true;
            slot.name_size = namespace_name.size;
            memcpy(slot.name, namespace_name.data, namespace_name.size);
            namespace_count += 1;
            stats.namespace_count = namespace_count;
        }
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

uint32_t on9kvdb::hash_key_unsafe(uint16_t namespace_slot_index, const uint8_t *key, size_t key_size) const
{
    uint32_t hash = UINT32_C(2166136261);
    hash ^= static_cast<uint8_t>(namespace_slot_index);
    hash *= UINT32_C(16777619);
    hash ^= static_cast<uint8_t>(namespace_slot_index >> 8U);
    hash *= UINT32_C(16777619);
    for (size_t idx = 0; idx < key_size; idx += 1) {
        hash ^= key[idx];
        hash *= UINT32_C(16777619);
    }
    return hash == 0 ? 1 : hash;
}

esp_err_t on9kvdb::find_memtable_bucket_unsafe(uint16_t namespace_slot_index, const uint8_t *key, size_t key_size,
                                               uint32_t *bucket_index_out, bool *found_out) const
{
    if (key == nullptr || key_size == 0 || key_size > on9kvdb_def::max_name_len || bucket_index_out == nullptr ||
        found_out == nullptr || namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t hash = hash_key_unsafe(namespace_slot_index, key, key_size);
    const uint32_t mask = CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT - 1U;
    for (uint32_t probe = 0; probe < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; probe += 1) {
        const uint32_t index = (hash + probe) & mask;
        const memtable_bucket &bucket = memtable_index[index];
        if (bucket.record_offset == UINT32_MAX) {
            *bucket_index_out = index;
            *found_out = false;
            return ESP_OK;
        }
        if (bucket.hash != hash) {
            continue;
        }
        if (bucket.record_size < sizeof(memtable_record_header) || bucket.record_size > CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE ||
            bucket.record_offset > CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE - bucket.record_size) {
            return ESP_ERR_INVALID_STATE;
        }

        const auto *header = reinterpret_cast<const memtable_record_header *>(memtable_data + bucket.record_offset);
        const bool external = (header->flags & on9kvdb_def::memtable_flag_external_value) != 0;
        const uint32_t inline_size = external ? 0 : header->value_size;
        if (header->total_size != bucket.record_size || header->key_size == 0 || header->key_size > on9kvdb_def::max_name_len ||
            inline_size > bucket.record_size - sizeof(memtable_record_header) ||
            header->key_size > bucket.record_size - sizeof(memtable_record_header) - inline_size ||
            header->namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES || !namespaces[header->namespace_slot_index].used ||
            (external && !value_ref_matches_bank_unsafe(header->external_value, manifest.active_value_bank,
                                                        manifest.value_bank_generation[manifest.active_value_bank],
                                                        manifest.value_bank_tail[manifest.active_value_bank]))) {
            return ESP_ERR_INVALID_STATE;
        }
        if (header->namespace_slot_index != namespace_slot_index) {
            continue;
        }
        const uint8_t *record_key = reinterpret_cast<const uint8_t *>(header + 1);
        if (header->key_size == key_size && memcmp(record_key, key, key_size) == 0) {
            *bucket_index_out = index;
            *found_out = true;
            return ESP_OK;
        }
    }

    // A full probe is a normal lookup miss. Callers that intend to insert distinguish it by the sentinel bucket index.
    *bucket_index_out = UINT32_MAX;
    *found_out = false;
    return ESP_OK;
}

esp_err_t on9kvdb::lookup_memtable_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const
{
    if (view_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t namespace_index = 0;
    esp_err_t ret = find_namespace_unsafe(namespace_name, &namespace_index);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!on9kvdb_def::validate_bytes(key.data, key.size)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t bucket_index = 0;
    bool found = false;
    ret = find_memtable_bucket_unsafe(namespace_index, key.data, key.size, &bucket_index, &found);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    const memtable_bucket &bucket = memtable_index[bucket_index];
    const auto *header = reinterpret_cast<const memtable_record_header *>(memtable_data + bucket.record_offset);
    const uint8_t *record_key = reinterpret_cast<const uint8_t *>(header + 1);
    value_view view = {};
    view.value = record_key + header->key_size;
    view.transaction_sequence = header->transaction_sequence;
    view.value_size = header->value_size;
    view.tombstone = (header->flags & on9kvdb_def::memtable_flag_tombstone) != 0;
    view.is_external = (header->flags & on9kvdb_def::memtable_flag_external_value) != 0;
    view.external_value = header->external_value;
    if (view.is_external) {
        view.value = nullptr;
    }
    *view_out = view;
    return ESP_OK;
}

const on9kvdb::mutation_slot *on9kvdb::find_staged_mutation_unsafe(const transaction_slot *transaction_slot_ptr,
                                                                   on9kvdb_bytes key) const
{
    if (transaction_slot_ptr == nullptr || !on9kvdb_def::validate_bytes(key.data, key.size)) {
        return nullptr;
    }

    for (uint16_t idx = 0; idx < transaction_slot_ptr->mutation_count; idx += 1) {
        if (transaction_slot_ptr->mutations[idx].key_size == key.size &&
            memcmp(transaction_slot_ptr->mutations[idx].key, key.data, key.size) == 0) {
            return &transaction_slot_ptr->mutations[idx];
        }
    }
    return nullptr;
}

on9kvdb::mutation_slot *on9kvdb::find_staged_mutation_unsafe(transaction_slot *transaction_slot_ptr, on9kvdb_bytes key)
{
    return const_cast<mutation_slot *>(
        static_cast<const on9kvdb *>(this)->find_staged_mutation_unsafe(transaction_slot_ptr, key));
}

esp_err_t on9kvdb::lookup_transaction_unsafe(const transaction_slot &transaction_state, const handle_slot &handle,
                                             on9kvdb_bytes key, value_view *view_out) const
{
    if (view_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const mutation_slot *mutation = find_staged_mutation_unsafe(&transaction_state, key);
    if (mutation != nullptr) {
        value_view view = {};
        view.tombstone = mutation->kind == on9kvdb_def::mutation_kind_tombstone;
        view.value_size = mutation->value_size;
        view.is_external = mutation->external_value;
        view.external_value = mutation->external_value_ref;
        if (mutation->value_size > 0 && !mutation->external_value) {
            view.value = transaction_staging + mutation->value_offset;
        }
        *view_out = view;
        return ESP_OK;
    }

    return lookup_committed_unsafe({handle.namespace_name, handle.namespace_size}, key, view_out);
}

void on9kvdb::compact_memtable_unsafe()
{
    // Removal shifts the complete tail and updates every affected bucket, so
    // the current Phase 3 arena is compact after each publication already.
}

void on9kvdb::remove_memtable_record_unsafe(uint32_t bucket_index)
{
    memtable_bucket &bucket = memtable_index[bucket_index];
    const uint32_t removed_offset = bucket.record_offset;
    const uint32_t removed_size = bucket.record_size;
    const uint32_t tail_offset = removed_offset + removed_size;
    const uint32_t tail_size = memtable_data_used - tail_offset;
    if (tail_size > 0) {
        memmove(memtable_data + removed_offset, memtable_data + tail_offset, tail_size);
    }

    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; idx += 1) {
        if (idx != bucket_index && memtable_index[idx].record_offset != UINT32_MAX &&
            memtable_index[idx].record_offset > removed_offset) {
            memtable_index[idx].record_offset -= removed_size;
        }
    }
    memtable_data_used -= removed_size;
    bucket.record_offset = removed_offset;
    bucket.record_size = 0;
}

esp_err_t on9kvdb::preflight_memtable_transaction_unsafe(transaction_slot &transaction_state, on9kvdb_bytes namespace_name,
                                                         uint16_t *namespace_slot_out)
{
    if (!on9kvdb_def::validate_bytes(namespace_name.data, namespace_name.size) || namespace_slot_out == nullptr ||
        transaction_state.mutation_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t namespace_index = 0;
    esp_err_t ret = ensure_namespace_capacity_unsafe(namespace_name, &namespace_index, false);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t final_bytes = memtable_data_used;
    uint32_t final_entries = memtable_entry_count;
    uint64_t final_logical_state_bytes = stats.logical_state_bytes;
    const uint32_t namespace_size = namespace_name.size;
    for (uint16_t idx = 0; idx < transaction_state.mutation_count; idx += 1) {
        mutation_slot &mutation = transaction_state.mutations[idx];
        mutation.previous_state = previous_state_none;
        mutation.previous_value_size = 0;
        uint32_t bucket_index = 0;
        bool found = false;
        ret = find_memtable_bucket_unsafe(namespace_index, mutation.key, mutation.key_size, &bucket_index, &found);
        if (ret != ESP_OK) {
            return ret;
        }
        if (found) {
            final_bytes -= memtable_index[bucket_index].record_size;
            const auto *old_header =
                reinterpret_cast<const memtable_record_header *>(memtable_data + memtable_index[bucket_index].record_offset);
            mutation.previous_state =
                (old_header->flags & on9kvdb_def::memtable_flag_tombstone) != 0 ? previous_state_tombstone : previous_state_live;
            mutation.previous_value_size = old_header->value_size;
        } else {
            value_view old_table_value = {};
            const esp_err_t table_ret = lookup_tables_unsafe(namespace_name, {mutation.key, mutation.key_size}, &old_table_value);
            if (table_ret == ESP_OK) {
                mutation.previous_state = old_table_value.tombstone ? previous_state_tombstone : previous_state_live;
                mutation.previous_value_size = old_table_value.value_size;
            } else if (table_ret != ESP_ERR_NOT_FOUND) {
                return table_ret;
            }
            final_entries += 1;
        }

        const uint32_t value_size = mutation.kind == on9kvdb_def::mutation_kind_set ? mutation.value_size : 0;
        const uint32_t memtable_value_size = mutation.external_value ? 0 : value_size;
        if (mutation.previous_state != previous_state_none) {
            const uint64_t previous_size = logical_record_size(namespace_size, mutation.key_size, mutation.previous_value_size);
            if (previous_size > final_logical_state_bytes) {
                return ESP_ERR_INVALID_STATE;
            }
            final_logical_state_bytes -= previous_size;
        }
        const uint32_t record_size =
            align_record_size(static_cast<uint32_t>(sizeof(memtable_record_header)) + mutation.key_size + memtable_value_size);
        const uint64_t new_logical_record_size = logical_record_size(namespace_size, mutation.key_size, value_size);
        if (record_size > CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE - final_bytes) {
            return ESP_ERR_NO_MEM;
        }
        final_bytes += record_size;
        final_logical_state_bytes += new_logical_record_size;
        if (final_logical_state_bytes > manifest.geometry.max_live_bytes) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (final_entries > CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) {
        return ESP_ERR_NO_MEM;
    }
    *namespace_slot_out = namespace_index;
    return ESP_OK;
}

esp_err_t on9kvdb::apply_transaction_to_memtable_unsafe(const transaction_slot &transaction_state, uint16_t namespace_slot_index,
                                                        uint64_t transaction_sequence)
{
    for (uint16_t idx = 0; idx < transaction_state.mutation_count; idx += 1) {
        const mutation_slot &mutation = transaction_state.mutations[idx];
        uint32_t bucket_index = 0;
        bool found = false;
        const esp_err_t find_ret =
            find_memtable_bucket_unsafe(namespace_slot_index, mutation.key, mutation.key_size, &bucket_index, &found);
        if (find_ret != ESP_OK) {
            return find_ret;
        }
        if (!found && bucket_index == UINT32_MAX) {
            return ESP_ERR_NO_MEM;
        }

        if (found) {
            const memtable_bucket &old_bucket = memtable_index[bucket_index];
            const auto *old_header = reinterpret_cast<const memtable_record_header *>(memtable_data + old_bucket.record_offset);
            const bool old_tombstone = (old_header->flags & on9kvdb_def::memtable_flag_tombstone) != 0;
            const uint64_t old_logical_size = logical_record_size(namespaces[old_header->namespace_slot_index].name_size,
                                                                  old_header->key_size, old_header->value_size);
            stats.logical_state_bytes -= old_logical_size;
            if (old_tombstone) {
                stats.tombstone_count -= 1;
            } else {
                stats.live_key_count -= 1;
                stats.logical_value_bytes -= old_header->value_size;
            }
            remove_memtable_record_unsafe(bucket_index);
        } else {
            const uint64_t previous_logical_size =
                logical_record_size(namespaces[namespace_slot_index].name_size, mutation.key_size, mutation.previous_value_size);
            if (mutation.previous_state == previous_state_tombstone) {
                stats.tombstone_count -= 1;
                stats.logical_state_bytes -= previous_logical_size;
            } else if (mutation.previous_state == previous_state_live) {
                stats.live_key_count -= 1;
                stats.logical_value_bytes -= mutation.previous_value_size;
                stats.logical_state_bytes -= previous_logical_size;
            } else if (mutation.previous_state != previous_state_none) {
                return ESP_ERR_INVALID_STATE;
            }
            memtable_entry_count += 1;
        }

        const bool tombstone = mutation.kind == on9kvdb_def::mutation_kind_tombstone;
        const uint32_t value_size = tombstone ? 0 : mutation.value_size;
        const uint32_t memtable_value_size = mutation.external_value ? 0 : value_size;
        const uint32_t record_size =
            align_record_size(static_cast<uint32_t>(sizeof(memtable_record_header)) + mutation.key_size + memtable_value_size);
        auto *header = reinterpret_cast<memtable_record_header *>(memtable_data + memtable_data_used);
        *header = {};
        header->transaction_sequence = transaction_sequence;
        header->total_size = record_size;
        header->value_size = value_size;
        header->namespace_slot_index = namespace_slot_index;
        header->key_size = mutation.key_size;
        header->reserved0 = mutation.reserved0;
        header->flags = tombstone ? on9kvdb_def::memtable_flag_tombstone : 0;
        if (mutation.external_value) {
            header->flags |= on9kvdb_def::memtable_flag_external_value;
            header->external_value = mutation.external_value_ref;
        }

        uint8_t *key_out = reinterpret_cast<uint8_t *>(header + 1);
        memcpy(key_out, mutation.key, mutation.key_size);
        if (value_size > 0 && !mutation.external_value) {
            memcpy(key_out + mutation.key_size, transaction_staging + mutation.value_offset, value_size);
        }
        const uint32_t meaningful_size =
            static_cast<uint32_t>(sizeof(memtable_record_header)) + mutation.key_size + memtable_value_size;
        if (record_size > meaningful_size) {
            memset(memtable_data + memtable_data_used + meaningful_size, 0, record_size - meaningful_size);
        }

        memtable_bucket &bucket = memtable_index[bucket_index];
        bucket.hash = hash_key_unsafe(namespace_slot_index, mutation.key, mutation.key_size);
        bucket.record_offset = memtable_data_used;
        bucket.record_size = record_size;
        memtable_data_used += record_size;
        stats.logical_state_bytes +=
            logical_record_size(namespaces[namespace_slot_index].name_size, mutation.key_size, value_size);
        if (tombstone) {
            stats.tombstone_count += 1;
        } else {
            stats.live_key_count += 1;
            stats.logical_value_bytes += value_size;
        }
    }

    stats.committed_transaction_count += 1;
    return ESP_OK;
}

#if 0 // Removed vNext typed getter implementation; values are read through on9kvdb_value_reader.
esp_err_t on9kvdb::get_fixed_value_unsafe(const value_view &view, on9kvdb_type expected_type, void *value_out,
                                          size_t expected_size) const
{
    if (value_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view.tombstone) {
        return ESP_ERR_NOT_FOUND;
    }
    if (view.type != expected_type || view.value_size != expected_size || view.value == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (expected_size == 1) {
        memcpy(value_out, view.value, expected_size);
        return ESP_OK;
    }

    if (expected_size == 2) {
        uint16_t decoded = 0;
        if (!on9kvdb_def::read_u16_le(view.value, view.value_size, 0, &decoded)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(value_out, &decoded, sizeof(decoded));
        return ESP_OK;
    }
    if (expected_size == 4) {
        uint32_t decoded = 0;
        if (!on9kvdb_def::read_u32_le(view.value, view.value_size, 0, &decoded)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(value_out, &decoded, sizeof(decoded));
        return ESP_OK;
    }
    if (expected_size == 8) {
        uint64_t decoded = 0;
        if (!on9kvdb_def::read_u64_le(view.value, view.value_size, 0, &decoded)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(value_out, &decoded, sizeof(decoded));
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t on9kvdb::get_variable_value_unsafe(const value_view &view, on9kvdb_type expected_type, void *value_out,
                                             size_t *length) const
{
    if (length == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view.tombstone) {
        return ESP_ERR_NOT_FOUND;
    }
    if (view.type != expected_type || (view.value == nullptr && view.value_size != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t required = view.value_size;
    if (value_out == nullptr) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_INVALID_SIZE;
    }
    if (required > 0) {
        memcpy(value_out, view.value, required);
    }
    *length = required;
    return ESP_OK;
}
#endif
