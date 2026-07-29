#include <cstring>

#include "on9kvdb.hpp"

namespace
{
    uint32_t align_table_entry_size(uint32_t size)
    {
        return (size + 7U) & ~UINT32_C(7);
    }

    bool valid_table_value(on9kvdb_type type, const uint8_t *value, uint32_t value_size, bool tombstone)
    {
        if (tombstone) {
            return type == on9kvdb_type::any && value_size == 0;
        }
        if (value == nullptr && value_size != 0) {
            return false;
        }

        switch (type) {
        case on9kvdb_type::i8:
        case on9kvdb_type::u8:
            return value_size == 1;
        case on9kvdb_type::i16:
        case on9kvdb_type::u16:
            return value_size == 2;
        case on9kvdb_type::i32:
        case on9kvdb_type::u32:
            return value_size == 4;
        case on9kvdb_type::i64:
        case on9kvdb_type::u64:
            return value_size == 8;
        case on9kvdb_type::str:
            return value_size > 0 && value[value_size - 1U] == '\0';
        case on9kvdb_type::blob:
            return true;
        default:
            return false;
        }
    }

    void copy_composite_key(on9kvdb_def::composite_key *key_out, const uint8_t *namespace_name, uint8_t namespace_size,
                            const uint8_t *key, uint8_t key_size)
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
}

esp_err_t on9kvdb::read_table_bytes_unsafe(uint32_t slot, uint64_t offset, uint8_t *destination, size_t size) const
{
    if (slot >= manifest.geometry.table_count || destination == nullptr || io_frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const int file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::table, slot)];
    if (destination == io_frame) {
        return size <= on9kvdb_def::wal_frame_size
                   ? read_exact_fd(file_fd, manifest.geometry.table_size, offset, destination, size)
                   : ESP_ERR_INVALID_SIZE;
    }
    size_t copied = 0;
    while (copied < size) {
        const size_t chunk = size - copied < on9kvdb_def::wal_frame_size ? size - copied : on9kvdb_def::wal_frame_size;
        esp_err_t ret = read_exact_fd(file_fd, manifest.geometry.table_size, offset + copied, io_frame, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        memcpy(destination + copied, io_frame, chunk);
        copied += chunk;
    }
    return ESP_OK;
}

esp_err_t on9kvdb::write_table_bytes_unsafe(uint32_t slot, uint64_t offset, const uint8_t *source, size_t size)
{
    if (slot >= manifest.geometry.table_count || source == nullptr || io_frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const int file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::table, slot)];
    if (source == io_frame) {
        return size <= on9kvdb_def::wal_frame_size ? write_exact_fd(file_fd, manifest.geometry.table_size, offset, source, size)
                                                   : ESP_ERR_INVALID_SIZE;
    }
    size_t copied = 0;
    while (copied < size) {
        const size_t chunk = size - copied < on9kvdb_def::wal_frame_size ? size - copied : on9kvdb_def::wal_frame_size;
        memcpy(io_frame, source + copied, chunk);
        const esp_err_t ret = write_exact_fd(file_fd, manifest.geometry.table_size, offset + copied, io_frame, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        copied += chunk;
    }
    return ESP_OK;
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
        if (memtable_index[bucket].record_offset != UINT32_MAX) {
            offsets[offset_count] = memtable_index[bucket].record_offset;
            offset_count += 1U;
        }
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
    state.file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::table, table_slot)];
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
        if (record->namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES || !namespaces[record->namespace_slot_index].used) {
            return ESP_ERR_INVALID_STATE;
        }
        const namespace_slot &record_namespace = namespaces[record->namespace_slot_index];
        const uint8_t *record_key = reinterpret_cast<const uint8_t *>(record + 1);
        const uint8_t *record_value = record_key + record->key_size;
        const uint32_t encoded_size = align_table_entry_size(on9kvdb_def::table_entry_header_size + record_namespace.name_size +
                                                             record->key_size + record->value_size);

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
        entry.type = record->type;
        entry.flags = (record->flags & on9kvdb_def::memtable_flag_tombstone) != 0 ? on9kvdb_def::table_entry_flag_tombstone : 0;
        entry.namespace_name = reinterpret_cast<const uint8_t *>(record_namespace.name);
        entry.key = record_key;
        entry.value = record_value;
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

    // The table body and footer become durable before either header. The manifest is published last, so recovery can never
    // observe a referenced table whose complete contents were not already synced and read back successfully.
    ret = sync_fd(state.file_fd);
    memset(io_frame, 0, on9kvdb_def::wal_frame_size);
    if (ret == ESP_OK &&
        !on9kvdb_def::encode_table_metadata(io_frame, on9kvdb_def::wal_frame_size, on9kvdb_def::table_footer_magic, metadata)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = write_exact_fd(state.file_fd, manifest.geometry.table_size, footer_offset, io_frame,
                             on9kvdb_def::table_footer_slot_size);
    }
    if (ret == ESP_OK) {
        ret = sync_fd(state.file_fd);
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
        ret = write_exact_fd(state.file_fd, manifest.geometry.table_size, header_offset, io_frame,
                             on9kvdb_def::table_header_slot_size);
        if (ret == ESP_OK) {
            ret = sync_fd(state.file_fd);
        }
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

esp_err_t on9kvdb::validate_table_unsafe(const on9kvdb_def::table_reference &reference)
{
    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    if (!reference.active || reference.slot >= manifest.geometry.table_count ||
        future_scratch_size < sort_bytes + 2U * manifest.limits.sstable_block_bytes) {
        return ESP_ERR_INVALID_ARG;
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
            if (entry.transaction_sequence < footer.min_sequence || entry.transaction_sequence > footer.max_sequence ||
                !valid_table_value(static_cast<on9kvdb_type>(entry.type), entry.value, entry.value_size, tombstone)) {
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
        total_entries != footer.entry_count || total_data_bytes != footer.data_bytes || ~content_crc != footer.content_checksum ||
        !on9kvdb_def::composite_key_equal(footer.min_key, reference.min_key) ||
        !on9kvdb_def::composite_key_equal(previous_key, reference.max_key)) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
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

    uint32_t selected_block_offset = UINT32_MAX;
    uint32_t offset = on9kvdb_def::table_index_header_size;
    for (uint32_t idx = 0; idx < index_header.entry_count; idx += 1) {
        on9kvdb_def::table_index_entry index_entry = {};
        if (on9kvdb_def::decode_table_index_entry(index_block, manifest.limits.sstable_block_bytes, offset, &index_entry) !=
            on9kvdb_def::format_status::ok) {
            return ESP_ERR_INVALID_CRC;
        }
        on9kvdb_def::composite_key first_key = {};
        copy_composite_key(&first_key, index_entry.namespace_name, index_entry.namespace_size, index_entry.key,
                           index_entry.key_size);
        if (on9kvdb_def::compare_composite_key(first_key, key) > 0) {
            break;
        }
        selected_block_offset = index_entry.block_offset;
        offset += index_entry.total_size;
    }
    if (selected_block_offset == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = read_table_bytes_unsafe(reference.slot, selected_block_offset, data_block, manifest.limits.sstable_block_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    on9kvdb_def::table_block_header block_header = {};
    if (on9kvdb_def::decode_table_block_header(data_block, manifest.limits.sstable_block_bytes, &block_header) !=
            on9kvdb_def::format_status::ok ||
        block_header.generation != reference.generation) {
        return ESP_ERR_INVALID_CRC;
    }

    offset = on9kvdb_def::table_block_header_size;
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
            view.type = static_cast<on9kvdb_type>(entry.type);
            view.tombstone = (entry.flags & on9kvdb_def::table_entry_flag_tombstone) != 0;
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

esp_err_t on9kvdb::lookup_tables_unsafe(const char *namespace_name, const char *key, value_view *view_out) const
{
    if (view_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t namespace_size = 0;
    size_t key_size = 0;
    if (!on9kvdb_def::validate_name(namespace_name, &namespace_size) || !on9kvdb_def::validate_name(key, &key_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    on9kvdb_def::composite_key composite = {};
    copy_composite_key(&composite, reinterpret_cast<const uint8_t *>(namespace_name), static_cast<uint8_t>(namespace_size),
                       reinterpret_cast<const uint8_t *>(key), static_cast<uint8_t>(key_size));
    uint64_t best_sequence = 0;
    uint32_t best_slot = manifest.geometry.table_count;
    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1) {
        const on9kvdb_def::table_reference &reference = manifest.tables[slot];
        if (!reference.active || on9kvdb_def::compare_composite_key(composite, reference.min_key) < 0 ||
            on9kvdb_def::compare_composite_key(composite, reference.max_key) > 0) {
            continue;
        }
        value_view candidate = {};
        const esp_err_t ret = lookup_table_unsafe(reference, composite, &candidate);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            return ret;
        }
        if (ret == ESP_OK && candidate.transaction_sequence > best_sequence) {
            best_sequence = candidate.transaction_sequence;
            best_slot = slot;
        }
    }
    if (best_slot == manifest.geometry.table_count) {
        return ESP_ERR_NOT_FOUND;
    }

    // Each table lookup reuses the same bounded block buffer. Re-read the winning table so the returned value remains valid
    // until the caller finishes copying it while still holding the operation lock.
    return lookup_table_unsafe(manifest.tables[best_slot], composite, view_out);
}

esp_err_t on9kvdb::lookup_committed_unsafe(const char *namespace_name, const char *key, value_view *view_out) const
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
        const on9kvdb_def::table_reference &reference = manifest.tables[slot];
        if (!reference.active) {
            continue;
        }
        const esp_err_t ret = validate_table_unsafe(reference);
        if (ret != ESP_OK) {
            return ret;
        }
        stats.table_bytes_used += on9kvdb_def::table_header_region_size +
                                  static_cast<uint64_t>(reference.data_block_count + 1U) * manifest.limits.sstable_block_bytes +
                                  on9kvdb_def::table_footer_slot_size;
    }

    // Rebuild namespace and logical statistics from immutable records. The bounded implementation deliberately accepts a
    // slower startup here: each record is counted only when no newer table contains the same composite key.
    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    uint8_t *data_block = future_scratch + sort_bytes;
    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1) {
        const on9kvdb_def::table_reference &reference = manifest.tables[slot];
        if (!reference.active) {
            continue;
        }
        for (uint32_t block = 0; block < reference.data_block_count; block += 1) {
            const uint32_t block_offset = on9kvdb_def::table_data_region_offset + block * manifest.limits.sstable_block_bytes;
            esp_err_t ret = read_table_bytes_unsafe(slot, block_offset, data_block, manifest.limits.sstable_block_bytes);
            if (ret != ESP_OK) {
                return ret;
            }
            on9kvdb_def::table_block_header header = {};
            if (on9kvdb_def::decode_table_block_header(data_block, manifest.limits.sstable_block_bytes, &header) !=
                on9kvdb_def::format_status::ok) {
                return ESP_ERR_INVALID_CRC;
            }

            uint32_t entry_offset = on9kvdb_def::table_block_header_size;
            for (uint16_t entry_index = 0; entry_index < header.entry_count; entry_index += 1) {
                on9kvdb_def::table_entry entry = {};
                if (on9kvdb_def::decode_table_entry(data_block, manifest.limits.sstable_block_bytes, entry_offset, &entry) !=
                    on9kvdb_def::format_status::ok) {
                    return ESP_ERR_INVALID_CRC;
                }
                char namespace_name[on9kvdb_def::max_name_len + 1] = {};
                char key[on9kvdb_def::max_name_len + 1] = {};
                memcpy(namespace_name, entry.namespace_name, entry.namespace_size);
                memcpy(key, entry.key, entry.key_size);
                uint16_t namespace_index = 0;
                ret = ensure_namespace_capacity_unsafe(namespace_name, &namespace_index, true);
                if (ret != ESP_OK) {
                    return ret;
                }

                value_view newest = {};
                ret = lookup_tables_unsafe(namespace_name, key, &newest);
                if (ret != ESP_OK) {
                    return ret;
                }
                if (newest.transaction_sequence == entry.transaction_sequence) {
                    stats.logical_state_bytes += align_table_entry_size(
                        on9kvdb_def::table_entry_header_size + entry.namespace_size + entry.key_size + newest.value_size);
                    if (newest.tombstone) {
                        stats.tombstone_count += 1U;
                    } else {
                        stats.live_key_count += 1U;
                        stats.logical_value_bytes += newest.value_size;
                    }
                }
                entry_offset += entry.total_size;

                // lookup_tables_unsafe reuses data_block, so reload the source block before decoding its next record.
                if (entry_index + 1U < header.entry_count) {
                    ret = read_table_bytes_unsafe(slot, block_offset, data_block, manifest.limits.sstable_block_bytes);
                    if (ret != ESP_OK) {
                        return ret;
                    }
                }
            }
        }
    }
    return ESP_OK;
}
