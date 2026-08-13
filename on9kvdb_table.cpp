#include <cstring>
#include <new>

#include "on9kvdb.hpp"

#include <freertos/task.h>

namespace
{
    static const constexpr uint32_t recovery_entry_delay_interval = 8;

    uint32_t align_table_entry_size(uint32_t size)
    {
        return (size + 7U) & ~UINT32_C(7);
    }

    bool valid_table_value(const uint8_t *value, uint32_t value_size, bool tombstone, bool external)
    {
        if (tombstone) {
            return value_size == 0 && !external;
        }
        return external || value_size == 0 || value != nullptr;
    }

    void copy_composite_key(on9kvdb_def::composite_key *key_out, const uint8_t *namespace_name, uint16_t namespace_size,
                            const uint8_t *key, uint16_t key_size)
    {
        *key_out = {};
        key_out->namespace_size = namespace_size;
        key_out->key_size = key_size;
        memcpy(key_out->namespace_name, namespace_name, namespace_size);
        memcpy(key_out->key, key, key_size);
    }

    on9kvdb_def::table_reference make_table_reference(const on9kvdb_def::table_metadata &metadata)
    {
        on9kvdb_def::table_reference reference = {};
        reference.active = true;
        reference.level = metadata.level;
        reference.slot = metadata.slot;
        reference.data_block_count = metadata.data_block_count;
        reference.generation = metadata.generation;
        reference.min_sequence = metadata.min_sequence;
        reference.max_sequence = metadata.max_sequence;
        reference.entry_count = metadata.entry_count;
        reference.data_bytes = metadata.data_bytes;
        reference.content_checksum = metadata.content_checksum;
        reference.min_key = metadata.min_key;
        reference.max_key = metadata.max_key;
        return reference;
    }

    uint32_t filter_hash(const on9kvdb_def::composite_key &key)
    {
        uint32_t hash = UINT32_C(2166136261);
        hash = (hash ^ static_cast<uint8_t>(key.namespace_size)) * UINT32_C(16777619);
        for (uint16_t index = 0; index < key.namespace_size; index += 1U) {
            hash = (hash ^ key.namespace_name[index]) * UINT32_C(16777619);
        }
        hash = (hash ^ static_cast<uint8_t>(key.key_size)) * UINT32_C(16777619);
        for (uint16_t index = 0; index < key.key_size; index += 1U) {
            hash = (hash ^ key.key[index]) * UINT32_C(16777619);
        }
        return hash;
    }
}

esp_err_t on9kvdb::read_table_bytes_unsafe(uint32_t slot, uint64_t offset, uint8_t *destination, size_t size) const
{
    if (slot >= manifest.geometry.table_count || destination == nullptr || io_frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_storage_bytes_unsafe(on9kvdb_def::file_kind::table, slot, offset, destination, size);
}

esp_err_t on9kvdb::write_table_bytes_unsafe(uint32_t slot, uint64_t offset, const uint8_t *source, size_t size)
{
    if (slot >= manifest.geometry.table_count || source == nullptr || io_frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return write_storage_bytes_unsafe(on9kvdb_def::file_kind::table, slot, offset, source, size);
}

int on9kvdb::compare_memtable_records_unsafe(uint32_t lhs_offset, uint32_t rhs_offset) const
{
    const auto *lhs = reinterpret_cast<const memtable_record_header *>(memtable_data + lhs_offset);
    const auto *rhs = reinterpret_cast<const memtable_record_header *>(memtable_data + rhs_offset);
    const namespace_slot &lhs_namespace = namespaces[lhs->namespace_slot_index];
    const namespace_slot &rhs_namespace = namespaces[rhs->namespace_slot_index];
    on9kvdb_def::composite_key lhs_key = {};
    on9kvdb_def::composite_key rhs_key = {};
    copy_composite_key(&lhs_key, reinterpret_cast<const uint8_t *>(lhs_namespace.name), lhs_namespace.name_size,
                       reinterpret_cast<const uint8_t *>(lhs + 1), lhs->key_size);
    copy_composite_key(&rhs_key, reinterpret_cast<const uint8_t *>(rhs_namespace.name), rhs_namespace.name_size,
                       reinterpret_cast<const uint8_t *>(rhs + 1), rhs->key_size);
    return on9kvdb_def::compare_composite_key(lhs_key, rhs_key);
}

void on9kvdb::sift_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count, uint32_t root) const
{
    if (offsets == nullptr || count < 2) {
        return;
    }
    while (root <= (count - 2U) / 2U) {
        uint32_t child = root * 2U + 1U;
        if (child + 1U < count && compare_memtable_records_unsafe(offsets[child], offsets[child + 1U]) < 0) {
            child += 1U;
        }
        if (compare_memtable_records_unsafe(offsets[root], offsets[child]) >= 0) {
            return;
        }
        const uint32_t temporary = offsets[root];
        offsets[root] = offsets[child];
        offsets[child] = temporary;
        root = child;
    }
}

void on9kvdb::sort_memtable_offsets_unsafe(uint32_t *offsets, uint32_t count) const
{
    if (offsets == nullptr || count < 2) {
        return;
    }

    for (uint32_t root = count / 2U; root > 0; root -= 1U) {
        sift_memtable_offsets_unsafe(offsets, count, root - 1U);
    }
    for (uint32_t end = count; end > 1; end -= 1U) {
        const uint32_t temporary = offsets[0];
        offsets[0] = offsets[end - 1U];
        offsets[end - 1U] = temporary;
        sift_memtable_offsets_unsafe(offsets, end - 1U, 0);
    }
}

esp_err_t on9kvdb::finish_table_data_block_unsafe(table_build_state *state)
{
    if (state == nullptr || state->data_block_entry_count == 0 || state->data_payload_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    on9kvdb_def::table_block_header header = {};
    header.generation = state->generation;
    header.block_index = state->data_block_count;
    header.entry_count = state->data_block_entry_count;
    header.payload_size = state->data_payload_size;
    if (!on9kvdb_def::encode_table_block_header(state->data_block, manifest.limits.sstable_block_bytes, header)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t offset = on9kvdb_def::table_data_region_offset +
                            static_cast<uint64_t>(state->data_block_count) * manifest.limits.sstable_block_bytes;
    esp_err_t ret = write_table_bytes_unsafe(state->slot, offset, state->data_block, manifest.limits.sstable_block_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    state->content_crc =
        on9kvdb_def::calc_crc32_update(state->content_crc, state->data_block, manifest.limits.sstable_block_bytes);
    state->data_block_count += 1U;
    state->data_payload_size = 0;
    state->data_block_entry_count = 0;
    memset(state->data_block, 0, manifest.limits.sstable_block_bytes);
    return ESP_OK;
}

void on9kvdb::reset_memtable_unsafe()
{
    memtable_data_used = 0;
    memtable_entry_count = 0;
    memset(memtable_data, 0, CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE);
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; idx += 1) {
        memtable_index[idx] = {};
        memtable_index[idx].record_offset = UINT32_MAX;
    }
}

esp_err_t on9kvdb::flush_memtable_unsafe()
{
    if (memtable_entry_count == 0) {
        return ESP_OK;
    }
    if (manifest.next_table_generation == 0 || manifest.next_table_generation == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t bank_size = manifest.geometry.table_count / 2U;
    const uint32_t bank_start = manifest.active_table_bank * bank_size;
    const uint32_t bank_end = bank_start + bank_size;
    uint32_t table_slot = bank_end;
    for (uint32_t slot = bank_start; slot < bank_end; slot += 1) {
        if (!manifest.tables[slot].active) {
            table_slot = slot;
            break;
        }
    }
    if (table_slot == bank_end) {
        return compact_tables_unsafe();
    }
    invalidate_table_index_cache_unsafe(table_slot);

    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    const size_t required_scratch = sort_bytes + 2U * manifest.limits.sstable_block_bytes;
    if (future_scratch == nullptr || future_scratch_size < required_scratch) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t table_generation = manifest.next_table_generation;
    esp_err_t ret = ESP_OK;

    auto *offsets = reinterpret_cast<uint32_t *>(future_scratch);
    uint8_t *data_block = future_scratch + sort_bytes;
    uint8_t *index_block = data_block + manifest.limits.sstable_block_bytes;
    uint32_t offset_count = 0;
    for (uint32_t bucket = 0; bucket < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; bucket += 1) {
        const memtable_bucket &entry = memtable_index[bucket];
        if (entry.record_offset == UINT32_MAX) {
            continue;
        }
        if (entry.record_size < sizeof(memtable_record_header) || entry.record_size > CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE ||
            entry.record_offset > CONFIG_ON9KVDB_MEMTABLE_DATA_SIZE - entry.record_size) {
            return ESP_ERR_INVALID_STATE;
        }
        const auto *record = reinterpret_cast<const memtable_record_header *>(memtable_data + entry.record_offset);
        const uint32_t record_header_bytes = static_cast<uint32_t>(sizeof(memtable_record_header));
        const bool external = (record->flags & on9kvdb_def::memtable_flag_external_value) != 0;
        const uint32_t inline_value_size = external ? 0 : record->value_size;
        if (record->total_size != entry.record_size || record->key_size == 0 || record->key_size > on9kvdb_def::max_name_len ||
            inline_value_size > entry.record_size - record_header_bytes ||
            record->key_size > entry.record_size - record_header_bytes - inline_value_size ||
            (external && !value_ref_matches_bank_unsafe(record->external_value, manifest.active_value_bank,
                                                        manifest.value_bank_generation[manifest.active_value_bank],
                                                        manifest.value_bank_tail[manifest.active_value_bank])) ||
            record->namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES || !namespaces[record->namespace_slot_index].used) {
            return ESP_ERR_INVALID_STATE;
        }
        offsets[offset_count] = entry.record_offset;
        offset_count += 1U;
    }
    if (offset_count != memtable_entry_count) {
        return ESP_ERR_INVALID_STATE;
    }
    sort_memtable_offsets_unsafe(offsets, offset_count);
    memset(data_block, 0, manifest.limits.sstable_block_bytes);
    memset(index_block, 0, manifest.limits.sstable_block_bytes);

    const uint32_t footer_offset = manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size;
    const uint32_t index_offset = footer_offset - manifest.limits.sstable_block_bytes;
    const uint32_t maximum_data_blocks =
        (index_offset - on9kvdb_def::table_data_region_offset) / manifest.limits.sstable_block_bytes;

    table_build_state state = {};
    state.data_block = data_block;
    state.index_block = index_block;
    state.generation = table_generation;
    state.slot = table_slot;

    on9kvdb_def::table_metadata metadata = {};
    metadata.database_id = manifest.database_id;
    metadata.generation = state.generation;
    metadata.slot = table_slot;
    metadata.block_size = manifest.limits.sstable_block_bytes;
    metadata.data_region_start = on9kvdb_def::table_data_region_offset;
    metadata.index_offset = index_offset;
    metadata.footer_offset = footer_offset;

    for (uint32_t idx = 0; idx < offset_count; idx += 1) {
        const auto *record = reinterpret_cast<const memtable_record_header *>(memtable_data + offsets[idx]);
        const namespace_slot &record_namespace = namespaces[record->namespace_slot_index];
        const uint8_t *record_key = reinterpret_cast<const uint8_t *>(record + 1);
        const uint8_t *record_value = record_key + record->key_size;
        const bool external = (record->flags & on9kvdb_def::memtable_flag_external_value) != 0;
        const uint32_t encoded_value_size = external ? on9kvdb_def::value_ref_encoded_size : record->value_size;
        const uint32_t encoded_size = align_table_entry_size(on9kvdb_def::table_entry_header_size + record_namespace.name_size +
                                                             record->key_size + encoded_value_size);

        if (state.data_block_entry_count > 0 &&
            encoded_size > manifest.limits.sstable_block_bytes - on9kvdb_def::table_block_header_size - state.data_payload_size) {
            ret = finish_table_data_block_unsafe(&state);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        if (state.data_block_entry_count == 0) {
            if (state.data_block_count >= maximum_data_blocks) {
                return ESP_ERR_NO_MEM;
            }

            on9kvdb_def::table_index_entry index_entry = {};
            index_entry.first_sequence = record->transaction_sequence;
            index_entry.block_offset =
                on9kvdb_def::table_data_region_offset + state.data_block_count * manifest.limits.sstable_block_bytes;
            index_entry.namespace_size = record_namespace.name_size;
            index_entry.key_size = record->key_size;
            index_entry.namespace_name = reinterpret_cast<const uint8_t *>(record_namespace.name);
            index_entry.key = record_key;
            size_t index_entry_size = 0;
            if (!on9kvdb_def::encode_table_index_entry(index_block, manifest.limits.sstable_block_bytes,
                                                       on9kvdb_def::table_index_header_size + state.index_payload_size,
                                                       index_entry, &index_entry_size)) {
                return ESP_ERR_NO_MEM;
            }
            state.index_payload_size += static_cast<uint32_t>(index_entry_size);
        }

        on9kvdb_def::table_entry entry = {};
        entry.transaction_sequence = record->transaction_sequence;
        entry.value_size = record->value_size;
        entry.namespace_size = record_namespace.name_size;
        entry.key_size = record->key_size;
        entry.reserved0 = record->reserved0;
        entry.flags = (record->flags & on9kvdb_def::memtable_flag_tombstone) != 0 ? on9kvdb_def::table_entry_flag_tombstone : 0;
        if (external) {
            entry.flags |= on9kvdb_def::table_entry_flag_external_value;
            entry.external_value = record->external_value;
        }
        entry.namespace_name = reinterpret_cast<const uint8_t *>(record_namespace.name);
        entry.key = record_key;
        entry.value = external ? nullptr : record_value;
        size_t entry_size = 0;
        if (!on9kvdb_def::encode_table_entry(data_block, manifest.limits.sstable_block_bytes,
                                             on9kvdb_def::table_block_header_size + state.data_payload_size, entry,
                                             &entry_size)) {
            return ESP_ERR_INVALID_SIZE;
        }

        on9kvdb_def::composite_key composite = {};
        copy_composite_key(&composite, entry.namespace_name, entry.namespace_size, entry.key, entry.key_size);
        if (idx == 0) {
            metadata.min_key = composite;
            metadata.min_sequence = entry.transaction_sequence;
            metadata.max_sequence = entry.transaction_sequence;
        }
        metadata.max_key = composite;
        if (entry.transaction_sequence < metadata.min_sequence) {
            metadata.min_sequence = entry.transaction_sequence;
        }
        if (entry.transaction_sequence > metadata.max_sequence) {
            metadata.max_sequence = entry.transaction_sequence;
        }

        state.data_payload_size += static_cast<uint32_t>(entry_size);
        state.data_block_entry_count += 1U;
        state.entry_count += 1U;
        state.data_bytes += static_cast<uint32_t>(entry_size);
    }

    ret = finish_table_data_block_unsafe(&state);
    if (ret != ESP_OK) {
        return ret;
    }
    on9kvdb_def::table_index_header index_header = {};
    index_header.generation = state.generation;
    index_header.entry_count = state.data_block_count;
    index_header.payload_size = state.index_payload_size;
    index_header.data_block_count = state.data_block_count;
    if (!on9kvdb_def::encode_table_index_header(index_block, manifest.limits.sstable_block_bytes, index_header)) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = write_table_bytes_unsafe(table_slot, index_offset, index_block, manifest.limits.sstable_block_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    state.content_crc = on9kvdb_def::calc_crc32_update(state.content_crc, index_block, manifest.limits.sstable_block_bytes);

    metadata.data_block_count = state.data_block_count;
    metadata.entry_count = state.entry_count;
    metadata.data_bytes = state.data_bytes;
    metadata.content_checksum = ~state.content_crc;

    // The table remains unreachable until the final manifest publication. Write its complete body and redundant metadata,
    // then make all of it durable with one file sync before validation and publication.
    memset(io_frame, 0, on9kvdb_def::wal_frame_size);
    if (!on9kvdb_def::encode_table_metadata(io_frame, on9kvdb_def::wal_frame_size, on9kvdb_def::table_footer_magic, metadata)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = write_storage_bytes_unsafe(on9kvdb_def::file_kind::table, table_slot, footer_offset, io_frame,
                                         on9kvdb_def::table_footer_slot_size);
    }
    for (uint32_t copy = 0; ret == ESP_OK && copy < on9kvdb_def::table_header_slot_count; copy += 1) {
        memset(io_frame, 0, on9kvdb_def::wal_frame_size);
        if (!on9kvdb_def::encode_table_metadata(io_frame, on9kvdb_def::wal_frame_size, on9kvdb_def::table_header_magic,
                                                metadata)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        const uint64_t header_offset =
            on9kvdb_def::table_header_region_offset + static_cast<uint64_t>(copy) * on9kvdb_def::table_header_slot_size;
        ret = write_storage_bytes_unsafe(on9kvdb_def::file_kind::table, table_slot, header_offset, io_frame,
                                         on9kvdb_def::table_header_slot_size);
    }
    if (ret == ESP_OK) {
        ret = sync_storage_unsafe();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    const on9kvdb_def::table_reference reference = make_table_reference(metadata);
    ret = validate_table_unsafe(reference);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint64_t reservation_checkpoint = manifest.safe_checkpoint_sequence;
    const uint64_t previous_next_table_generation = manifest.next_table_generation;
    manifest.tables[table_slot] = reference;
    manifest.safe_checkpoint_sequence = metadata.max_sequence;
    manifest.next_table_generation += 1U;
    ret = write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_ready);
    if (ret != ESP_OK) {
        manifest.tables[table_slot] = {};
        manifest.safe_checkpoint_sequence = reservation_checkpoint;
        manifest.next_table_generation = previous_next_table_generation;
        return ret;
    }

    stats.table_bytes_used += on9kvdb_def::table_header_region_size +
                              static_cast<uint64_t>(state.data_block_count + 1U) * manifest.limits.sstable_block_bytes +
                              on9kvdb_def::table_footer_slot_size;
    reset_memtable_unsafe();
    return ESP_OK;
}

esp_err_t on9kvdb::validate_table_unsafe(const on9kvdb_def::table_reference &reference, uint32_t value_bank,
                                         uint64_t value_bank_generation, uint32_t value_bank_tail)
{
    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    if (!reference.active || reference.slot >= manifest.geometry.table_count ||
        future_scratch_size < sort_bytes + 2U * manifest.limits.sstable_block_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    invalidate_table_index_cache_unsafe(reference.slot);
    reset_table_key_filter_unsafe(reference.slot);
    if (value_bank == UINT32_MAX) {
        value_bank = manifest.active_value_bank;
        if (value_bank >= on9kvdb_def::value_bank_count) {
            return ESP_ERR_INVALID_CRC;
        }
        value_bank_generation = manifest.value_bank_generation[value_bank];
        value_bank_tail = manifest.value_bank_tail[value_bank];
    }
    if (value_bank >= on9kvdb_def::value_bank_count || value_bank_generation == 0 ||
        value_bank_tail > manifest.geometry.value_bank_size) {
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t *data_block = future_scratch + sort_bytes;
    uint8_t *index_block = data_block + manifest.limits.sstable_block_bytes;
    on9kvdb_def::table_metadata footer = {};
    esp_err_t ret = read_table_bytes_unsafe(reference.slot, manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size,
                                            io_frame, on9kvdb_def::table_metadata_size);
    if (ret != ESP_OK) {
        return ret;
    }
    const on9kvdb_def::format_status footer_status =
        on9kvdb_def::decode_table_metadata(io_frame, on9kvdb_def::table_metadata_size, on9kvdb_def::table_footer_magic, &footer);
    if (footer_status == on9kvdb_def::format_status::new_version) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (footer_status != on9kvdb_def::format_status::ok ||
        !on9kvdb_def::table_reference_equal(reference, make_table_reference(footer)) ||
        footer.block_size != manifest.limits.sstable_block_bytes ||
        footer.footer_offset != manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t valid_headers = 0;
    for (uint32_t copy = 0; copy < on9kvdb_def::table_header_slot_count; copy += 1) {
        const uint64_t offset =
            on9kvdb_def::table_header_region_offset + static_cast<uint64_t>(copy) * on9kvdb_def::table_header_slot_size;
        ret = read_table_bytes_unsafe(reference.slot, offset, io_frame, on9kvdb_def::table_metadata_size);
        if (ret != ESP_OK) {
            return ret;
        }
        on9kvdb_def::table_metadata header = {};
        const on9kvdb_def::format_status status = on9kvdb_def::decode_table_metadata(io_frame, on9kvdb_def::table_metadata_size,
                                                                                     on9kvdb_def::table_header_magic, &header);
        if (status == on9kvdb_def::format_status::new_version) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (status == on9kvdb_def::format_status::ok) {
            if (!on9kvdb_def::table_metadata_equal(header, footer)) {
                return ESP_ERR_INVALID_CRC;
            }
            valid_headers += 1U;
        }
    }
    if (valid_headers == 0) {
        return ESP_ERR_INVALID_CRC;
    }

    ret = read_table_bytes_unsafe(reference.slot, footer.index_offset, index_block, footer.block_size);
    if (ret != ESP_OK) {
        return ret;
    }
    on9kvdb_def::table_index_header index_header = {};
    if (on9kvdb_def::decode_table_index_header(index_block, footer.block_size, &index_header) != on9kvdb_def::format_status::ok ||
        index_header.generation != footer.generation || index_header.data_block_count != footer.data_block_count) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t content_crc = UINT32_MAX;
    uint32_t index_entry_offset = on9kvdb_def::table_index_header_size;
    uint32_t total_entries = 0;
    uint32_t total_data_bytes = 0;
    on9kvdb_def::composite_key previous_key = {};
    bool have_previous_key = false;
    for (uint32_t block_index = 0; block_index < footer.data_block_count; block_index += 1) {
        on9kvdb_def::table_index_entry index_entry = {};
        if (on9kvdb_def::decode_table_index_entry(index_block, footer.block_size, index_entry_offset, &index_entry) !=
            on9kvdb_def::format_status::ok) {
            return ESP_ERR_INVALID_CRC;
        }
        const uint32_t expected_block_offset =
            on9kvdb_def::table_data_region_offset + block_index * manifest.limits.sstable_block_bytes;
        if (index_entry.block_offset != expected_block_offset) {
            return ESP_ERR_INVALID_CRC;
        }
        index_entry_offset += index_entry.total_size;

        ret = read_table_bytes_unsafe(reference.slot, expected_block_offset, data_block, footer.block_size);
        if (ret != ESP_OK) {
            return ret;
        }
        on9kvdb_def::table_block_header block_header = {};
        if (on9kvdb_def::decode_table_block_header(data_block, footer.block_size, &block_header) !=
                on9kvdb_def::format_status::ok ||
            block_header.generation != footer.generation || block_header.block_index != block_index) {
            return ESP_ERR_INVALID_CRC;
        }
        content_crc = on9kvdb_def::calc_crc32_update(content_crc, data_block, footer.block_size);

        uint32_t entry_offset = on9kvdb_def::table_block_header_size;
        for (uint16_t entry_index = 0; entry_index < block_header.entry_count; entry_index += 1) {
            on9kvdb_def::table_entry entry = {};
            if (on9kvdb_def::decode_table_entry(data_block, footer.block_size, entry_offset, &entry) !=
                on9kvdb_def::format_status::ok) {
                return ESP_ERR_INVALID_CRC;
            }
            const bool tombstone = (entry.flags & on9kvdb_def::table_entry_flag_tombstone) != 0;
            const bool external = (entry.flags & on9kvdb_def::table_entry_flag_external_value) != 0;
            if (entry.transaction_sequence < footer.min_sequence || entry.transaction_sequence > footer.max_sequence ||
                entry.reserved0 != 0 || !valid_table_value(entry.value, entry.value_size, tombstone, external) ||
                (external &&
                 !value_ref_matches_bank_unsafe(entry.external_value, value_bank, value_bank_generation, value_bank_tail))) {
                return ESP_ERR_INVALID_CRC;
            }

            on9kvdb_def::composite_key current_key = {};
            copy_composite_key(&current_key, entry.namespace_name, entry.namespace_size, entry.key, entry.key_size);
            if ((have_previous_key && on9kvdb_def::compare_composite_key(previous_key, current_key) >= 0) ||
                (entry_index == 0 &&
                 (index_entry.namespace_size != entry.namespace_size || index_entry.key_size != entry.key_size ||
                  memcmp(index_entry.namespace_name, entry.namespace_name, entry.namespace_size) != 0 ||
                  memcmp(index_entry.key, entry.key, entry.key_size) != 0 ||
                  index_entry.first_sequence != entry.transaction_sequence))) {
                return ESP_ERR_INVALID_CRC;
            }
            previous_key = current_key;
            have_previous_key = true;
            add_table_key_filter_unsafe(reference.slot, current_key);
            entry_offset += entry.total_size;
            total_data_bytes += entry.total_size;
            total_entries += 1U;
        }
        if (entry_offset != on9kvdb_def::table_block_header_size + block_header.payload_size) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    content_crc = on9kvdb_def::calc_crc32_update(content_crc, index_block, footer.block_size);
    if (index_entry_offset != on9kvdb_def::table_index_header_size + index_header.payload_size ||
        total_entries != footer.entry_count || total_data_bytes != footer.data_bytes || ~content_crc != footer.content_checksum) {
        return ESP_ERR_INVALID_CRC;
    }
    table_key_filters[reference.slot].generation = reference.generation;
    table_key_filters[reference.slot].valid = true;
    return cache_table_index_unsafe(reference, index_block);
}

esp_err_t on9kvdb::cache_table_index_unsafe(const on9kvdb_def::table_reference &reference, const uint8_t *index_block)
{
    if (!reference.active || reference.slot >= manifest.geometry.table_count || index_block == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (table_index_cache == nullptr) {
        return ESP_OK;
    }
    if (reference.data_block_count == 0 || reference.data_block_count > maximum_table_data_blocks) {
        return ESP_ERR_INVALID_CRC;
    }

    table_index_cache_slot &cached = table_index_cache[reference.slot];
    cached = {};
    on9kvdb_def::table_index_header header = {};
    if (on9kvdb_def::decode_table_index_header(index_block, manifest.limits.sstable_block_bytes, &header) !=
            on9kvdb_def::format_status::ok ||
        header.generation != reference.generation || header.entry_count != reference.data_block_count ||
        header.data_block_count != reference.data_block_count) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t offset = on9kvdb_def::table_index_header_size;
    for (uint32_t idx = 0; idx < header.entry_count; idx += 1U) {
        on9kvdb_def::table_index_entry entry = {};
        if (on9kvdb_def::decode_table_index_entry(index_block, manifest.limits.sstable_block_bytes, offset, &entry) !=
            on9kvdb_def::format_status::ok) {
            return ESP_ERR_INVALID_CRC;
        }
        table_index_cache_entry &destination = cached.entries[idx];
        copy_composite_key(&destination.first_key, entry.namespace_name, entry.namespace_size, entry.key, entry.key_size);
        destination.block_offset = entry.block_offset;
        offset += entry.total_size;
    }
    if (offset != on9kvdb_def::table_index_header_size + header.payload_size) {
        return ESP_ERR_INVALID_CRC;
    }

    cached.generation = reference.generation;
    cached.entry_count = header.entry_count;
    cached.valid = true;
    return ESP_OK;
}

void on9kvdb::invalidate_table_index_cache_unsafe(uint32_t slot)
{
    if (table_index_cache != nullptr && slot < manifest.geometry.table_count) {
        table_index_cache[slot] = {};
    }
}

void on9kvdb::reset_table_key_filter_unsafe(uint32_t slot)
{
    if (table_key_filters != nullptr && slot < manifest.geometry.table_count) {
        table_key_filters[slot] = {};
    }
}

void on9kvdb::add_table_key_filter_unsafe(uint32_t slot, const on9kvdb_def::composite_key &key)
{
    if (table_key_filters == nullptr || slot >= manifest.geometry.table_count) {
        return;
    }
    table_key_filter_slot &filter = table_key_filters[slot];
    const uint32_t first = filter_hash(key);
    const uint32_t step = ((first >> 16U) ^ UINT32_C(0x9e3779b9)) | 1U;
    const uint32_t bit_count = static_cast<uint32_t>(table_key_filter_bytes * 8U);
    for (uint32_t probe = 0; probe < 3U; probe += 1U) {
        const uint32_t bit = (first + probe * step) % bit_count;
        filter.bits[bit / 8U] |= static_cast<uint8_t>(UINT8_C(1) << (bit % 8U));
    }
}

bool on9kvdb::table_key_filter_may_contain_unsafe(const on9kvdb_def::table_reference &reference,
                                                  const on9kvdb_def::composite_key &key) const
{
    if (table_key_filters == nullptr || reference.slot >= manifest.geometry.table_count) {
        return true;
    }
    const table_key_filter_slot &filter = table_key_filters[reference.slot];
    if (!filter.valid || filter.generation != reference.generation) {
        return true;
    }
    const uint32_t first = filter_hash(key);
    const uint32_t step = ((first >> 16U) ^ UINT32_C(0x9e3779b9)) | 1U;
    const uint32_t bit_count = static_cast<uint32_t>(table_key_filter_bytes * 8U);
    for (uint32_t probe = 0; probe < 3U; probe += 1U) {
        const uint32_t bit = (first + probe * step) % bit_count;
        if ((filter.bits[bit / 8U] & static_cast<uint8_t>(UINT8_C(1) << (bit % 8U))) == 0) {
            return false;
        }
    }
    return true;
}

esp_err_t on9kvdb::lookup_table_unsafe(const on9kvdb_def::table_reference &reference, const on9kvdb_def::composite_key &key,
                                       value_view *view_out) const
{
    if (view_out == nullptr || !reference.active) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    uint8_t *data_block = future_scratch + sort_bytes;
    uint8_t *index_block = data_block + manifest.limits.sstable_block_bytes;
    uint32_t selected_block_offset = UINT32_MAX;
    const table_index_cache_slot *cached = nullptr;
    if (table_index_cache != nullptr && reference.slot < manifest.geometry.table_count) {
        const table_index_cache_slot &candidate = table_index_cache[reference.slot];
        if (candidate.valid && candidate.generation == reference.generation &&
            candidate.entry_count == reference.data_block_count) {
            cached = &candidate;
        }
    }
    if (cached != nullptr) {
        for (uint32_t idx = 0; idx < cached->entry_count; idx += 1U) {
            if (on9kvdb_def::compare_composite_key(cached->entries[idx].first_key, key) > 0) {
                break;
            }
            selected_block_offset = cached->entries[idx].block_offset;
        }
    } else {
        const uint32_t footer_offset = manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size;
        const uint32_t index_offset = footer_offset - manifest.limits.sstable_block_bytes;
        esp_err_t ret = read_table_bytes_unsafe(reference.slot, index_offset, index_block, manifest.limits.sstable_block_bytes);
        if (ret != ESP_OK) {
            return ret;
        }
        on9kvdb_def::table_index_header index_header = {};
        if (on9kvdb_def::decode_table_index_header(index_block, manifest.limits.sstable_block_bytes, &index_header) !=
                on9kvdb_def::format_status::ok ||
            index_header.generation != reference.generation || index_header.data_block_count != reference.data_block_count) {
            return ESP_ERR_INVALID_CRC;
        }

        uint32_t index_entry_offset = on9kvdb_def::table_index_header_size;
        for (uint32_t idx = 0; idx < index_header.entry_count; idx += 1U) {
            on9kvdb_def::table_index_entry index_entry = {};
            if (on9kvdb_def::decode_table_index_entry(index_block, manifest.limits.sstable_block_bytes, index_entry_offset,
                                                      &index_entry) != on9kvdb_def::format_status::ok) {
                return ESP_ERR_INVALID_CRC;
            }
            on9kvdb_def::composite_key first_key = {};
            copy_composite_key(&first_key, index_entry.namespace_name, index_entry.namespace_size, index_entry.key,
                               index_entry.key_size);
            if (on9kvdb_def::compare_composite_key(first_key, key) > 0) {
                break;
            }
            selected_block_offset = index_entry.block_offset;
            index_entry_offset += index_entry.total_size;
        }
    }
    if (selected_block_offset == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret =
        read_table_bytes_unsafe(reference.slot, selected_block_offset, data_block, manifest.limits.sstable_block_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    on9kvdb_def::table_block_header block_header = {};
    if (on9kvdb_def::decode_table_block_header(data_block, manifest.limits.sstable_block_bytes, &block_header) !=
            on9kvdb_def::format_status::ok ||
        block_header.generation != reference.generation) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t offset = on9kvdb_def::table_block_header_size;
    for (uint16_t idx = 0; idx < block_header.entry_count; idx += 1) {
        on9kvdb_def::table_entry entry = {};
        if (on9kvdb_def::decode_table_entry(data_block, manifest.limits.sstable_block_bytes, offset, &entry) !=
            on9kvdb_def::format_status::ok) {
            return ESP_ERR_INVALID_CRC;
        }
        on9kvdb_def::composite_key entry_key = {};
        copy_composite_key(&entry_key, entry.namespace_name, entry.namespace_size, entry.key, entry.key_size);
        const int comparison = on9kvdb_def::compare_composite_key(entry_key, key);
        if (comparison == 0) {
            value_view view = {};
            view.value = entry.value;
            view.transaction_sequence = entry.transaction_sequence;
            view.value_size = entry.value_size;
            view.tombstone = (entry.flags & on9kvdb_def::table_entry_flag_tombstone) != 0;
            view.is_external = (entry.flags & on9kvdb_def::table_entry_flag_external_value) != 0;
            view.external_value = entry.external_value;
            *view_out = view;
            return ESP_OK;
        }
        if (comparison > 0) {
            break;
        }
        offset += entry.total_size;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t on9kvdb::lookup_tables_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const
{
    if (view_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!on9kvdb_def::validate_bytes(namespace_name.data, namespace_name.size) ||
        !on9kvdb_def::validate_bytes(key.data, key.size)) {
        return ESP_ERR_INVALID_ARG;
    }

    on9kvdb_def::composite_key composite = {};
    copy_composite_key(&composite, namespace_name.data, namespace_name.size, key.data, key.size);
    uint64_t best_sequence = 0;
    uint32_t best_slot = manifest.geometry.table_count;
    uint32_t examined_slots = 0;
    value_view best = {};
    // Examine plausible tables by descending maximum sequence. This commonly finds the newest version first and lets older
    // tables be skipped without reading either their index or data blocks. The fixed 16-slot mask keeps this allocation-free.
    for (uint32_t pass = 0; pass < manifest.geometry.table_count; pass += 1U) {
        uint32_t slot = manifest.geometry.table_count;
        uint64_t greatest_max_sequence = 0;
        for (uint32_t candidate_slot = 0; candidate_slot < manifest.geometry.table_count; candidate_slot += 1U) {
            const uint32_t candidate_bit = UINT32_C(1) << candidate_slot;
            const on9kvdb_def::table_reference &candidate = manifest.tables[candidate_slot];
            if ((examined_slots & candidate_bit) != 0 || !candidate.active ||
                on9kvdb_def::compare_composite_key(composite, candidate.min_key) < 0 ||
                on9kvdb_def::compare_composite_key(composite, candidate.max_key) > 0 ||
                !table_key_filter_may_contain_unsafe(candidate, composite)) {
                continue;
            }
            if (slot == manifest.geometry.table_count || candidate.max_sequence > greatest_max_sequence) {
                slot = candidate_slot;
                greatest_max_sequence = candidate.max_sequence;
            }
        }
        if (slot == manifest.geometry.table_count || greatest_max_sequence < best_sequence) {
            break;
        }
        examined_slots |= UINT32_C(1) << slot;
        const on9kvdb_def::table_reference &reference = manifest.tables[slot];
        value_view candidate = {};
        const esp_err_t ret = lookup_table_unsafe(reference, composite, &candidate);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            return ret;
        }
        if (ret == ESP_OK && candidate.transaction_sequence > best_sequence) {
            best_sequence = candidate.transaction_sequence;
            best_slot = slot;
            if (table_lookup_value != nullptr) {
                if ((!candidate.is_external && candidate.value_size > on9kvdb_def::inline_value_len) ||
                    (candidate.value_size != 0 && candidate.value == nullptr)) {
                    if (!candidate.is_external) {
                        return ESP_ERR_INVALID_CRC;
                    }
                }
                if (!candidate.is_external && candidate.value_size != 0) {
                    memcpy(table_lookup_value, candidate.value, candidate.value_size);
                }
                best = candidate;
                best.value = candidate.is_external || candidate.value_size == 0 ? nullptr : table_lookup_value;
            }
        }
    }
    if (best_slot == manifest.geometry.table_count) {
        return ESP_ERR_NOT_FOUND;
    }

    if (table_lookup_value != nullptr) {
        *view_out = best;
        return ESP_OK;
    }

    // Without the optional stable result buffer, re-read the winning table so its value remains valid until the caller copies
    // it under the operation lock.
    return lookup_table_unsafe(manifest.tables[best_slot], composite, view_out);
}

esp_err_t on9kvdb::lookup_committed_unsafe(on9kvdb_bytes namespace_name, on9kvdb_bytes key, value_view *view_out) const
{
    const esp_err_t memtable_ret = lookup_memtable_unsafe(namespace_name, key, view_out);
    if (memtable_ret != ESP_ERR_NOT_FOUND) {
        return memtable_ret;
    }
    return lookup_tables_unsafe(namespace_name, key, view_out);
}

esp_err_t on9kvdb::recover_tables_unsafe()
{
    stats.table_bytes_used = 0;
    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1) {
        on9kvdb_def::table_reference &reference = manifest.tables[slot];
        if (!reference.active) {
            continue;
        }
        const esp_err_t ret = validate_table_unsafe(reference);
        if (ret != ESP_OK) {
            return ret;
        }
        const uint32_t footer_offset = manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size;
        esp_err_t read_ret =
            read_table_bytes_unsafe(reference.slot, footer_offset, io_frame, on9kvdb_def::table_footer_slot_size);
        if (read_ret != ESP_OK) {
            return read_ret;
        }
        on9kvdb_def::table_metadata footer = {};
        if (on9kvdb_def::decode_table_metadata(io_frame, on9kvdb_def::table_footer_slot_size, on9kvdb_def::table_footer_magic,
                                               &footer) != on9kvdb_def::format_status::ok ||
            footer.generation != reference.generation || footer.slot != reference.slot) {
            return ESP_ERR_INVALID_CRC;
        }
        reference.min_key = footer.min_key;
        reference.max_key = footer.max_key;
        stats.table_bytes_used += on9kvdb_def::table_header_region_size +
                                  static_cast<uint64_t>(reference.data_block_count + 1U) * manifest.limits.sstable_block_bytes +
                                  on9kvdb_def::table_footer_slot_size;
        // A yield does not allow the lower-priority idle task to run while recovery remains ready.
        vTaskDelay(1);
    }

    // Every active table is sorted by composite key and was fully validated above. Merge one bounded cursor per table,
    // count only the greatest-sequence version of each key, and advance every cursor carrying that key. This avoids a
    // random lookup across all tables for every immutable record.
    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    const size_t cursor_offset = sort_bytes + 2U * manifest.limits.sstable_block_bytes;
    const uint32_t maximum_cursor_count = manifest.geometry.table_count / 2U;
    const size_t required_scratch = cursor_offset + static_cast<size_t>(maximum_cursor_count) * sizeof(compaction_cursor);
    if (future_scratch == nullptr || future_scratch_size < required_scratch) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *block_validation_buffer = future_scratch + sort_bytes;
    auto *cursors = reinterpret_cast<compaction_cursor *>(future_scratch + cursor_offset);
    uint32_t cursor_count = 0;
    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1U) {
        if (!manifest.tables[slot].active) {
            continue;
        }
        if (cursor_count >= maximum_cursor_count) {
            return ESP_ERR_INVALID_CRC;
        }
        new (&cursors[cursor_count]) compaction_cursor{};
        cursors[cursor_count].source_slot = slot;
        const esp_err_t ret = load_compaction_cursor_unsafe(&cursors[cursor_count], block_validation_buffer);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!cursors[cursor_count].active) {
            return ESP_ERR_INVALID_CRC;
        }
        cursor_count += 1U;
    }

    uint32_t entries_since_delay = 0;
    while (cursor_count > 0) {
        bool found = false;
        on9kvdb_def::composite_key smallest = {};
        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1U) {
            if (cursors[cursor_index].active &&
                (!found || on9kvdb_def::compare_composite_key(cursors[cursor_index].key, smallest) < 0)) {
                smallest = cursors[cursor_index].key;
                found = true;
            }
        }
        if (!found) {
            return ESP_ERR_INVALID_CRC;
        }

        const compaction_cursor *winner = nullptr;
        uint64_t winner_sequence = 0;
        uint32_t matching_cursor_count = 0;
        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1U) {
            compaction_cursor &candidate = cursors[cursor_index];
            if (!candidate.active || !on9kvdb_def::composite_key_equal(candidate.key, smallest)) {
                continue;
            }
            for (uint32_t previous_index = 0; previous_index < cursor_index; previous_index += 1U) {
                const compaction_cursor &previous = cursors[previous_index];
                if (previous.active && previous.transaction_sequence == candidate.transaction_sequence &&
                    on9kvdb_def::composite_key_equal(previous.key, smallest)) {
                    return ESP_ERR_INVALID_CRC;
                }
            }
            matching_cursor_count += 1U;
            if (winner == nullptr || candidate.transaction_sequence > winner_sequence) {
                winner = &candidate;
                winner_sequence = candidate.transaction_sequence;
            }
        }
        if (winner == nullptr || matching_cursor_count == 0) {
            return ESP_ERR_INVALID_CRC;
        }

        uint16_t namespace_index = 0;
        esp_err_t ret =
            ensure_namespace_capacity_unsafe({winner->key.namespace_name, winner->key.namespace_size}, &namespace_index, true);
        if (ret != ESP_OK) {
            return ret;
        }
        const uint64_t logical_entry_size = (static_cast<uint64_t>(on9kvdb_def::table_entry_header_size) +
                                             winner->key.namespace_size + winner->key.key_size + winner->value_size + 7U) &
                                            ~UINT64_C(7);
        if (logical_entry_size > manifest.geometry.max_live_bytes - stats.logical_state_bytes) {
            return ESP_ERR_INVALID_CRC;
        }
        stats.logical_state_bytes += logical_entry_size;
        if ((winner->flags & on9kvdb_def::table_entry_flag_tombstone) != 0) {
            stats.tombstone_count += 1U;
        } else {
            stats.live_key_count += 1U;
            stats.logical_value_bytes += winner->value_size;
        }

        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1U) {
            if (cursors[cursor_index].active && on9kvdb_def::composite_key_equal(cursors[cursor_index].key, smallest)) {
                ret = advance_compaction_cursor_unsafe(&cursors[cursor_index], block_validation_buffer);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }
        while (cursor_count > 0 && !cursors[cursor_count - 1U].active) {
            cursor_count -= 1U;
        }

        entries_since_delay += matching_cursor_count;
        if (entries_since_delay >= recovery_entry_delay_interval) {
            vTaskDelay(1);
            entries_since_delay = 0;
        }
    }
    if (entries_since_delay != 0) {
        vTaskDelay(1);
    }
    return ESP_OK;
}
