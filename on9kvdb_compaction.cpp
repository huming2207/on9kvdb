#include <cstring>
#include <new>

#include <esp_timer.h>

#include "on9kvdb.hpp"

#include <freertos/task.h>

namespace
{
    uint32_t align_entry_size(uint32_t size)
    {
        return (size + 7U) & ~UINT32_C(7);
    }

    uint64_t logical_entry_size(uint16_t namespace_size, uint16_t key_size, uint32_t value_size)
    {
        const uint64_t size =
            static_cast<uint64_t>(on9kvdb_def::table_entry_header_size) + namespace_size + key_size + value_size;
        return (size + 7U) & ~UINT64_C(7);
    }

    void copy_composite_key(on9kvdb_def::composite_key *destination, const uint8_t *namespace_name, uint8_t namespace_size,
                            const uint8_t *key, uint8_t key_size)
    {
        *destination = {};
        destination->namespace_size = namespace_size;
        destination->key_size = key_size;
        memcpy(destination->namespace_name, namespace_name, namespace_size);
        memcpy(destination->key, key, key_size);
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

    uint64_t saturating_add(uint64_t lhs, uint64_t rhs)
    {
        return rhs > UINT64_MAX - lhs ? UINT64_MAX : lhs + rhs;
    }
}

esp_err_t on9kvdb::load_compaction_cursor_unsafe(compaction_cursor *cursor, uint8_t *block_validation_buffer)
{
    if (cursor == nullptr || cursor->source_slot >= manifest.geometry.table_count ||
        !manifest.tables[cursor->source_slot].active) {
        return ESP_ERR_INVALID_ARG;
    }

    // Input tables are fully validated before the merge. Cursors retain only the current key and disk position so the merge
    // does not need one 12-KiB block buffer per input table.
    const on9kvdb_def::table_reference &reference = manifest.tables[cursor->source_slot];
    if (cursor->block_entry_count == 0) {
        if (cursor->block_index >= reference.data_block_count) {
            cursor->active = false;
            return ESP_OK;
        }

        const uint32_t block_offset =
            on9kvdb_def::table_data_region_offset + cursor->block_index * manifest.limits.sstable_block_bytes;
        uint16_t entry_count = 0;
        uint32_t payload_size = 0;
        if (block_validation_buffer != nullptr) {
            esp_err_t ret = read_table_bytes_unsafe(cursor->source_slot, block_offset, block_validation_buffer,
                                                    manifest.limits.sstable_block_bytes);
            if (ret != ESP_OK) {
                return ret;
            }
            on9kvdb_def::table_block_header header = {};
            if (on9kvdb_def::decode_table_block_header(block_validation_buffer, manifest.limits.sstable_block_bytes, &header) !=
                    on9kvdb_def::format_status::ok ||
                header.generation != reference.generation || header.block_index != cursor->block_index) {
                return ESP_ERR_INVALID_CRC;
            }
            entry_count = header.entry_count;
            payload_size = header.payload_size;
        } else {
            esp_err_t ret =
                read_table_bytes_unsafe(cursor->source_slot, block_offset, io_frame, on9kvdb_def::table_block_header_size);
            if (ret != ESP_OK) {
                return ret;
            }

            uint32_t magic = 0;
            uint16_t revision = 0;
            uint16_t header_size = 0;
            uint64_t generation = 0;
            uint32_t encoded_block_index = 0;
            uint16_t reserved = 0;
            if (!on9kvdb_def::read_u32_le(io_frame, on9kvdb_def::table_block_header_size, 0, &magic) ||
                !on9kvdb_def::read_u16_le(io_frame, on9kvdb_def::table_block_header_size, 4, &revision) ||
                !on9kvdb_def::read_u16_le(io_frame, on9kvdb_def::table_block_header_size, 6, &header_size) ||
                !on9kvdb_def::read_u64_le(io_frame, on9kvdb_def::table_block_header_size, 8, &generation) ||
                !on9kvdb_def::read_u32_le(io_frame, on9kvdb_def::table_block_header_size, 16, &encoded_block_index) ||
                !on9kvdb_def::read_u16_le(io_frame, on9kvdb_def::table_block_header_size, 20, &entry_count) ||
                !on9kvdb_def::read_u16_le(io_frame, on9kvdb_def::table_block_header_size, 22, &reserved) ||
                !on9kvdb_def::read_u32_le(io_frame, on9kvdb_def::table_block_header_size, 24, &payload_size) ||
                magic != on9kvdb_def::table_block_magic || revision != on9kvdb_def::table_block_revision ||
                header_size != on9kvdb_def::table_block_header_size || generation != reference.generation ||
                encoded_block_index != cursor->block_index || entry_count == 0 || reserved != 0 || payload_size == 0 ||
                payload_size > manifest.limits.sstable_block_bytes - on9kvdb_def::table_block_header_size) {
                return ESP_ERR_INVALID_CRC;
            }
        }

        cursor->entry_index = 0;
        cursor->entry_offset = on9kvdb_def::table_block_header_size;
        cursor->block_entry_count = entry_count;
        cursor->block_payload_end = on9kvdb_def::table_block_header_size + payload_size;
    }

    const uint32_t absolute_offset =
        on9kvdb_def::table_data_region_offset + cursor->block_index * manifest.limits.sstable_block_bytes + cursor->entry_offset;
    esp_err_t ret = read_table_bytes_unsafe(cursor->source_slot, absolute_offset, io_frame, on9kvdb_def::table_entry_header_size);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t total_size = 0;
    uint32_t value_size = 0;
    uint64_t transaction_sequence = 0;
    if (!on9kvdb_def::read_u32_le(io_frame, on9kvdb_def::table_entry_header_size, 0, &total_size) ||
        !on9kvdb_def::read_u32_le(io_frame, on9kvdb_def::table_entry_header_size, 4, &value_size) ||
        !on9kvdb_def::read_u64_le(io_frame, on9kvdb_def::table_entry_header_size, 8, &transaction_sequence)) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t namespace_size = io_frame[16];
    const uint8_t key_size = io_frame[17];
    const uint8_t reserved0 = io_frame[18];
    const uint8_t flags = io_frame[19];
    const bool external = (flags & on9kvdb_def::table_entry_flag_external_value) != 0;
    const uint32_t encoded_value_size = external ? on9kvdb_def::value_ref_encoded_size : value_size;
    const uint32_t meaningful_size = on9kvdb_def::table_entry_header_size + namespace_size + key_size + encoded_value_size;
    if (namespace_size == 0 || namespace_size > on9kvdb_def::max_name_len || key_size == 0 ||
        key_size > on9kvdb_def::max_name_len || reserved0 != 0 || value_size > on9kvdb_def::max_value_len ||
        transaction_sequence == 0 || total_size < meaningful_size || total_size % 8U != 0 ||
        cursor->entry_offset > cursor->block_payload_end || total_size > cursor->block_payload_end - cursor->entry_offset ||
        (flags & ~(on9kvdb_def::table_entry_flag_tombstone | on9kvdb_def::table_entry_flag_external_value)) != 0 ||
        ((flags & on9kvdb_def::table_entry_flag_tombstone) != 0 && value_size != 0)) {
        return ESP_ERR_INVALID_CRC;
    }

    const size_t name_bytes = static_cast<size_t>(namespace_size) + key_size;
    const size_t descriptor_bytes = external ? on9kvdb_def::value_ref_encoded_size : 0;
    ret = read_table_bytes_unsafe(cursor->source_slot, absolute_offset + on9kvdb_def::table_entry_header_size,
                                  io_frame + on9kvdb_def::table_entry_header_size, name_bytes + descriptor_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint8_t *namespace_name = io_frame + on9kvdb_def::table_entry_header_size;
    const uint8_t *key = namespace_name + namespace_size;
    copy_composite_key(&cursor->key, namespace_name, namespace_size, key, key_size);
    cursor->transaction_sequence = transaction_sequence;
    cursor->total_size = total_size;
    cursor->value_size = value_size;
    cursor->reserved0 = reserved0;
    cursor->flags = flags;
    cursor->external_value = {};
    if (external) {
        const uint8_t *encoded_ref = key + key_size;
        cursor->external_value.bank_slot = encoded_ref[0];
        if (encoded_ref[1] != 0 || encoded_ref[2] != 0 || encoded_ref[3] != 0 ||
            !on9kvdb_def::read_u64_le(encoded_ref, descriptor_bytes, 4, &cursor->external_value.bank_generation) ||
            !on9kvdb_def::read_u32_le(encoded_ref, descriptor_bytes, 12, &cursor->external_value.first_chunk_offset) ||
            !on9kvdb_def::read_u32_le(encoded_ref, descriptor_bytes, 16, &cursor->external_value.value_checksum) ||
            encoded_ref[20] != 0 || encoded_ref[21] != 0 || encoded_ref[22] != 0 || encoded_ref[23] != 0) {
            return ESP_ERR_INVALID_CRC;
        }
        cursor->external_value.value_size = value_size;
        if (!on9kvdb_def::value_ref_is_valid(cursor->external_value, manifest.geometry.value_bank_size)) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    cursor->active = true;
    return ESP_OK;
}

esp_err_t on9kvdb::advance_compaction_cursor_unsafe(compaction_cursor *cursor, uint8_t *block_validation_buffer)
{
    if (cursor == nullptr || !cursor->active || cursor->entry_index >= cursor->block_entry_count) {
        return ESP_ERR_INVALID_ARG;
    }

    cursor->entry_offset += cursor->total_size;
    cursor->entry_index += 1U;
    if (cursor->entry_index == cursor->block_entry_count) {
        if (cursor->entry_offset != cursor->block_payload_end) {
            return ESP_ERR_INVALID_CRC;
        }
        cursor->block_index += 1U;
        cursor->block_entry_count = 0;
    }
    return load_compaction_cursor_unsafe(cursor, block_validation_buffer);
}

esp_err_t on9kvdb::start_compaction_output_unsafe(compaction_output *output, uint32_t slot, uint64_t generation,
                                                  uint8_t *data_block, uint8_t *index_block)
{
    if (output == nullptr || data_block == nullptr || index_block == nullptr || slot >= manifest.geometry.table_count ||
        generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *output = {};
    invalidate_table_index_cache_unsafe(slot);
    output->build.file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::table, slot)];
    output->build.data_block = data_block;
    output->build.index_block = index_block;
    output->build.generation = generation;
    output->build.slot = slot;
    output->metadata.database_id = manifest.database_id;
    output->metadata.generation = generation;
    output->metadata.slot = slot;
    output->metadata.block_size = manifest.limits.sstable_block_bytes;
    output->metadata.data_region_start = on9kvdb_def::table_data_region_offset;
    output->metadata.footer_offset = manifest.geometry.table_size - on9kvdb_def::table_footer_slot_size;
    output->metadata.index_offset = output->metadata.footer_offset - manifest.limits.sstable_block_bytes;
    output->maximum_data_blocks =
        (output->metadata.index_offset - on9kvdb_def::table_data_region_offset) / manifest.limits.sstable_block_bytes;
    output->active = true;
    memset(data_block, 0, manifest.limits.sstable_block_bytes);
    memset(index_block, 0, manifest.limits.sstable_block_bytes);
    return ESP_OK;
}

esp_err_t on9kvdb::append_compaction_entry_unsafe(compaction_output *output, const compaction_cursor *table_cursor,
                                                  const memtable_record_header *memtable_record,
                                                  const namespace_slot *memtable_namespace, uint32_t destination_value_bank,
                                                  uint64_t destination_value_generation, uint32_t *destination_value_tail)
{
    if (output == nullptr || !output->active || ((table_cursor == nullptr) == (memtable_record == nullptr)) ||
        (memtable_record != nullptr && memtable_namespace == nullptr) || destination_value_tail == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    on9kvdb_def::composite_key key = {};
    uint64_t transaction_sequence = 0;
    uint32_t encoded_size = 0;
    if (table_cursor != nullptr) {
        key = table_cursor->key;
        transaction_sequence = table_cursor->transaction_sequence;
        encoded_size = table_cursor->total_size;
    } else {
        const uint8_t *record_key = reinterpret_cast<const uint8_t *>(memtable_record + 1);
        copy_composite_key(&key, reinterpret_cast<const uint8_t *>(memtable_namespace->name), memtable_namespace->name_size,
                           record_key, memtable_record->key_size);
        transaction_sequence = memtable_record->transaction_sequence;
        const bool external = (memtable_record->flags & on9kvdb_def::memtable_flag_external_value) != 0;
        encoded_size =
            align_entry_size(on9kvdb_def::table_entry_header_size + memtable_namespace->name_size + memtable_record->key_size +
                             (external ? on9kvdb_def::value_ref_encoded_size : memtable_record->value_size));
    }

    if (output->build.data_block_entry_count > 0 && encoded_size > manifest.limits.sstable_block_bytes -
                                                                       on9kvdb_def::table_block_header_size -
                                                                       output->build.data_payload_size) {
        const esp_err_t ret = finish_table_data_block_unsafe(&output->build);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (output->build.data_block_entry_count == 0) {
        if (output->build.data_block_count >= output->maximum_data_blocks) {
            return ESP_ERR_NO_MEM;
        }

        on9kvdb_def::table_index_entry index_entry = {};
        index_entry.first_sequence = transaction_sequence;
        index_entry.block_offset =
            on9kvdb_def::table_data_region_offset + output->build.data_block_count * manifest.limits.sstable_block_bytes;
        index_entry.namespace_size = key.namespace_size;
        index_entry.key_size = key.key_size;
        index_entry.namespace_name = reinterpret_cast<const uint8_t *>(key.namespace_name);
        index_entry.key = reinterpret_cast<const uint8_t *>(key.key);
        size_t index_entry_size = 0;
        if (!on9kvdb_def::encode_table_index_entry(output->build.index_block, manifest.limits.sstable_block_bytes,
                                                   on9kvdb_def::table_index_header_size + output->build.index_payload_size,
                                                   index_entry, &index_entry_size)) {
            return ESP_ERR_NO_MEM;
        }
        output->build.index_payload_size += static_cast<uint32_t>(index_entry_size);
    }

    const uint32_t destination_offset = on9kvdb_def::table_block_header_size + output->build.data_payload_size;
    size_t actual_size = 0;
    if (table_cursor != nullptr) {
        const uint32_t source_offset = on9kvdb_def::table_data_region_offset +
                                       table_cursor->block_index * manifest.limits.sstable_block_bytes +
                                       table_cursor->entry_offset;
        esp_err_t ret = read_table_bytes_unsafe(table_cursor->source_slot, source_offset,
                                                output->build.data_block + destination_offset, encoded_size);
        if (ret != ESP_OK) {
            return ret;
        }
        on9kvdb_def::table_entry decoded = {};
        if (on9kvdb_def::decode_table_entry(output->build.data_block, manifest.limits.sstable_block_bytes, destination_offset,
                                            &decoded) != on9kvdb_def::format_status::ok ||
            decoded.total_size != encoded_size || decoded.transaction_sequence != transaction_sequence) {
            return ESP_ERR_INVALID_CRC;
        }
        if ((decoded.flags & on9kvdb_def::table_entry_flag_external_value) != 0) {
            on9kvdb_def::value_ref relocated = {};
            ret = copy_external_value_unsafe(decoded.external_value, destination_value_bank, destination_value_generation,
                                             destination_value_tail, &relocated);
            if (ret != ESP_OK) {
                return ret;
            }
            decoded.external_value = relocated;
            if (!on9kvdb_def::encode_table_entry(output->build.data_block, manifest.limits.sstable_block_bytes,
                                                 destination_offset, decoded, &actual_size)) {
                return ESP_ERR_INVALID_STATE;
            }
        } else {
            actual_size = decoded.total_size;
        }
    } else {
        const uint8_t *record_key = reinterpret_cast<const uint8_t *>(memtable_record + 1);
        on9kvdb_def::table_entry entry = {};
        entry.transaction_sequence = memtable_record->transaction_sequence;
        entry.value_size = memtable_record->value_size;
        entry.namespace_size = memtable_namespace->name_size;
        entry.key_size = memtable_record->key_size;
        entry.reserved0 = memtable_record->reserved0;
        entry.flags =
            (memtable_record->flags & on9kvdb_def::memtable_flag_tombstone) != 0 ? on9kvdb_def::table_entry_flag_tombstone : 0;
        entry.namespace_name = reinterpret_cast<const uint8_t *>(memtable_namespace->name);
        entry.key = record_key;
        const bool external = (memtable_record->flags & on9kvdb_def::memtable_flag_external_value) != 0;
        if (external) {
            entry.flags |= on9kvdb_def::table_entry_flag_external_value;
            const esp_err_t relocate_ret =
                copy_external_value_unsafe(memtable_record->external_value, destination_value_bank, destination_value_generation,
                                           destination_value_tail, &entry.external_value);
            if (relocate_ret != ESP_OK) {
                return relocate_ret;
            }
        }
        entry.value = external ? nullptr : record_key + memtable_record->key_size;
        if (!on9kvdb_def::encode_table_entry(output->build.data_block, manifest.limits.sstable_block_bytes, destination_offset,
                                             entry, &actual_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (output->build.entry_count == 0) {
        output->metadata.min_key = key;
        output->metadata.min_sequence = transaction_sequence;
        output->metadata.max_sequence = transaction_sequence;
    }
    output->metadata.max_key = key;
    if (transaction_sequence < output->metadata.min_sequence) {
        output->metadata.min_sequence = transaction_sequence;
    }
    if (transaction_sequence > output->metadata.max_sequence) {
        output->metadata.max_sequence = transaction_sequence;
    }
    output->build.data_payload_size += static_cast<uint32_t>(actual_size);
    output->build.data_block_entry_count += 1U;
    output->build.entry_count += 1U;
    output->build.data_bytes += static_cast<uint32_t>(actual_size);
    return ESP_OK;
}

esp_err_t on9kvdb::finish_compaction_output_unsafe(compaction_output *output, on9kvdb_def::table_reference *reference_out)
{
    if (output == nullptr || reference_out == nullptr || !output->active || output->build.entry_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (output->build.data_block_entry_count > 0) {
        ret = finish_table_data_block_unsafe(&output->build);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    on9kvdb_def::table_index_header index_header = {};
    index_header.generation = output->build.generation;
    index_header.entry_count = output->build.data_block_count;
    index_header.payload_size = output->build.index_payload_size;
    index_header.data_block_count = output->build.data_block_count;
    if (!on9kvdb_def::encode_table_index_header(output->build.index_block, manifest.limits.sstable_block_bytes, index_header)) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = write_table_bytes_unsafe(output->build.slot, output->metadata.index_offset, output->build.index_block,
                                   manifest.limits.sstable_block_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    output->build.content_crc =
        on9kvdb_def::calc_crc32_update(output->build.content_crc, output->build.index_block, manifest.limits.sstable_block_bytes);

    output->metadata.data_block_count = output->build.data_block_count;
    output->metadata.entry_count = output->build.entry_count;
    output->metadata.data_bytes = output->build.data_bytes;
    output->metadata.content_checksum = ~output->build.content_crc;

    memset(io_frame, 0, on9kvdb_def::wal_frame_size);
    if (!on9kvdb_def::encode_table_metadata(io_frame, on9kvdb_def::wal_frame_size, on9kvdb_def::table_footer_magic,
                                            output->metadata)) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        ret = write_exact_fd(output->build.file_fd, manifest.geometry.table_size, output->metadata.footer_offset, io_frame,
                             on9kvdb_def::table_footer_slot_size);
    }
    for (uint32_t copy = 0; ret == ESP_OK && copy < on9kvdb_def::table_header_slot_count; copy += 1) {
        memset(io_frame, 0, on9kvdb_def::wal_frame_size);
        if (!on9kvdb_def::encode_table_metadata(io_frame, on9kvdb_def::wal_frame_size, on9kvdb_def::table_header_magic,
                                                output->metadata)) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        const uint64_t header_offset =
            on9kvdb_def::table_header_region_offset + static_cast<uint64_t>(copy) * on9kvdb_def::table_header_slot_size;
        ret = write_exact_fd(output->build.file_fd, manifest.geometry.table_size, header_offset, io_frame,
                             on9kvdb_def::table_header_slot_size);
    }
    if (ret == ESP_OK) {
        // The output remains unreachable until the later manifest bank swap, so one sync can publish the complete file.
        ret = sync_fd(output->build.file_fd);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    const on9kvdb_def::table_reference reference = make_table_reference(output->metadata);
    ret = validate_table_unsafe(reference);
    if (ret == ESP_OK) {
        *reference_out = reference;
        output->active = false;
    }
    return ret;
}

esp_err_t on9kvdb::compact_tables_unsafe()
{
    const int64_t compaction_start_us = esp_timer_get_time();
    const uint32_t bank_size = manifest.geometry.table_count / 2U;
    const uint32_t input_start = manifest.active_table_bank * bank_size;
    const uint32_t output_start = (1U - manifest.active_table_bank) * bank_size;
    const uint32_t destination_value_bank = 1U - manifest.active_value_bank;
    const size_t sort_bytes = static_cast<size_t>(CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT) * sizeof(uint32_t);
    const size_t block_bytes = manifest.limits.sstable_block_bytes;
    const size_t cursor_offset = sort_bytes + 2U * block_bytes;
    const size_t required_scratch = cursor_offset + bank_size * sizeof(compaction_cursor);
    if (future_scratch == nullptr || future_scratch_size < required_scratch ||
        manifest.next_table_generation > UINT64_MAX - bank_size || manifest.generation > UINT64_MAX - 2U ||
        manifest.active_value_bank >= on9kvdb_def::value_bank_count ||
        manifest.value_bank_generation[destination_value_bank] == UINT64_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    // The destination bank is overwritten from its beginning. Readers publish direct pointers into their dedicated buffers,
    // so a reader retaining a value from the previous generation makes that bank unavailable for this compaction. A finished
    // but uncommitted writer is also a pin: it may hold a descriptor for an old bank which has not reached the memtable yet.
    if (value_bank_is_pinned_unsafe(destination_value_bank) ||
        value_bank_has_staged_reference_unsafe(destination_value_bank)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t destination_value_generation = manifest.value_bank_generation[destination_value_bank] + 1U;
    uint32_t destination_value_tail = on9kvdb_def::identity_region_size;

    for (uint32_t slot = input_start; slot < input_start + bank_size; slot += 1) {
        if (manifest.tables[slot].active) {
            const esp_err_t ret = validate_table_unsafe(manifest.tables[slot]);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    for (uint32_t slot = output_start; slot < output_start + bank_size; slot += 1) {
        if (manifest.tables[slot].active) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    auto *offsets = reinterpret_cast<uint32_t *>(future_scratch);
    uint8_t *data_block = future_scratch + sort_bytes;
    uint8_t *index_block = data_block + block_bytes;
    auto *cursors = reinterpret_cast<compaction_cursor *>(future_scratch + cursor_offset);

    uint32_t memtable_offset_count = 0;
    uint64_t input_record_bytes = 0;
    for (uint32_t slot = input_start; slot < input_start + bank_size; slot += 1) {
        if (manifest.tables[slot].active) {
            input_record_bytes += manifest.tables[slot].data_bytes;
        }
    }
    for (uint32_t bucket = 0; bucket < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; bucket += 1) {
        if (memtable_index[bucket].record_offset != UINT32_MAX) {
            offsets[memtable_offset_count] = memtable_index[bucket].record_offset;
            const auto *record =
                reinterpret_cast<const memtable_record_header *>(memtable_data + memtable_index[bucket].record_offset);
            if (record->namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES || !namespaces[record->namespace_slot_index].used) {
                return ESP_ERR_INVALID_STATE;
            }
            input_record_bytes +=
                align_entry_size(on9kvdb_def::table_entry_header_size + namespaces[record->namespace_slot_index].name_size +
                                 record->key_size + record->value_size);
            memtable_offset_count += 1U;
        }
    }
    if (memtable_offset_count != memtable_entry_count) {
        return ESP_ERR_INVALID_STATE;
    }
    sort_memtable_offsets_unsafe(offsets, memtable_offset_count);

    uint32_t cursor_count = 0;
    for (uint32_t slot = input_start; slot < input_start + bank_size; slot += 1) {
        if (!manifest.tables[slot].active) {
            continue;
        }
        new (&cursors[cursor_count]) compaction_cursor{};
        cursors[cursor_count].source_slot = slot;
        const esp_err_t ret = load_compaction_cursor_unsafe(&cursors[cursor_count], nullptr);
        if (ret != ESP_OK) {
            return ret;
        }
        cursor_count += 1U;
    }

    compaction_output output = {};
    uint32_t output_count = 0;
    uint32_t memtable_position = 0;
    uint64_t logical_state_bytes = 0;
    uint64_t greatest_sequence = 0;
    uint32_t processed_entries = 0;
    esp_err_t ret = ESP_OK;
    // All sources are sorted by composite key. Select the smallest key, retain its greatest transaction sequence, and advance
    // every source carrying that key. The newest tombstone is emitted like any other record.
    while (memtable_position < memtable_offset_count || cursor_count > 0) {
        bool found = false;
        on9kvdb_def::composite_key smallest = {};
        const memtable_record_header *memtable_record = nullptr;
        const namespace_slot *memtable_namespace = nullptr;
        if (memtable_position < memtable_offset_count) {
            memtable_record = reinterpret_cast<const memtable_record_header *>(memtable_data + offsets[memtable_position]);
            if (memtable_record->namespace_slot_index >= CONFIG_ON9KVDB_MAX_NAMESPACES ||
                !namespaces[memtable_record->namespace_slot_index].used) {
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            memtable_namespace = &namespaces[memtable_record->namespace_slot_index];
            const uint8_t *key = reinterpret_cast<const uint8_t *>(memtable_record + 1);
            copy_composite_key(&smallest, reinterpret_cast<const uint8_t *>(memtable_namespace->name),
                               memtable_namespace->name_size, key, memtable_record->key_size);
            found = true;
        }
        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1) {
            if (cursors[cursor_index].active &&
                (!found || on9kvdb_def::compare_composite_key(cursors[cursor_index].key, smallest) < 0)) {
                smallest = cursors[cursor_index].key;
                found = true;
            }
        }
        if (!found) {
            break;
        }

        const compaction_cursor *winner_cursor = nullptr;
        bool winner_is_memtable = false;
        uint64_t winner_sequence = 0;
        if (memtable_record != nullptr) {
            on9kvdb_def::composite_key memtable_key = {};
            const uint8_t *key = reinterpret_cast<const uint8_t *>(memtable_record + 1);
            copy_composite_key(&memtable_key, reinterpret_cast<const uint8_t *>(memtable_namespace->name),
                               memtable_namespace->name_size, key, memtable_record->key_size);
            if (on9kvdb_def::composite_key_equal(memtable_key, smallest)) {
                winner_sequence = memtable_record->transaction_sequence;
                winner_is_memtable = true;
            }
        }
        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1) {
            if (!cursors[cursor_index].active || !on9kvdb_def::composite_key_equal(cursors[cursor_index].key, smallest)) {
                continue;
            }
            if (cursors[cursor_index].transaction_sequence == winner_sequence) {
                ret = ESP_ERR_INVALID_CRC;
                break;
            }
            if (cursors[cursor_index].transaction_sequence > winner_sequence) {
                winner_sequence = cursors[cursor_index].transaction_sequence;
                winner_cursor = &cursors[cursor_index];
                winner_is_memtable = false;
            }
        }
        if (ret != ESP_OK) {
            break;
        }

        const uint64_t winner_logical_size =
            winner_is_memtable
                ? logical_entry_size(memtable_namespace->name_size, memtable_record->key_size, memtable_record->value_size)
                : logical_entry_size(winner_cursor->key.namespace_size, winner_cursor->key.key_size, winner_cursor->value_size);
        if (winner_logical_size > manifest.geometry.max_live_bytes - logical_state_bytes) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        logical_state_bytes += winner_logical_size;

        if (!output.active) {
            if (output_count >= bank_size) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            ret = start_compaction_output_unsafe(&output, output_start + output_count,
                                                 manifest.next_table_generation + output_count, data_block, index_block);
            if (ret != ESP_OK) {
                break;
            }
        }
        ret = append_compaction_entry_unsafe(&output, winner_is_memtable ? nullptr : winner_cursor,
                                             winner_is_memtable ? memtable_record : nullptr,
                                             winner_is_memtable ? memtable_namespace : nullptr, destination_value_bank,
                                             destination_value_generation, &destination_value_tail);
        if (ret == ESP_ERR_NO_MEM) {
            on9kvdb_def::table_reference reference = {};
            ret = finish_compaction_output_unsafe(&output, &reference);
            if (ret != ESP_OK) {
                break;
            }
            manifest.tables[output_start + output_count] = reference;
            output_count += 1U;
            if (output_count >= bank_size) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            ret = start_compaction_output_unsafe(&output, output_start + output_count,
                                                 manifest.next_table_generation + output_count, data_block, index_block);
            if (ret == ESP_OK) {
                ret = append_compaction_entry_unsafe(&output, winner_is_memtable ? nullptr : winner_cursor,
                                                     winner_is_memtable ? memtable_record : nullptr,
                                                     winner_is_memtable ? memtable_namespace : nullptr, destination_value_bank,
                                                     destination_value_generation, &destination_value_tail);
            }
        }
        if (ret != ESP_OK) {
            break;
        }
        if (winner_sequence > greatest_sequence) {
            greatest_sequence = winner_sequence;
        }

        if (memtable_record != nullptr) {
            on9kvdb_def::composite_key memtable_key = {};
            const uint8_t *key = reinterpret_cast<const uint8_t *>(memtable_record + 1);
            copy_composite_key(&memtable_key, reinterpret_cast<const uint8_t *>(memtable_namespace->name),
                               memtable_namespace->name_size, key, memtable_record->key_size);
            if (on9kvdb_def::composite_key_equal(memtable_key, smallest)) {
                memtable_position += 1U;
            }
        }
        for (uint32_t cursor_index = 0; cursor_index < cursor_count; cursor_index += 1) {
            if (cursors[cursor_index].active && on9kvdb_def::composite_key_equal(cursors[cursor_index].key, smallest)) {
                ret = advance_compaction_cursor_unsafe(&cursors[cursor_index], nullptr);
                if (ret != ESP_OK) {
                    break;
                }
            }
        }
        while (cursor_count > 0 && !cursors[cursor_count - 1U].active) {
            cursor_count -= 1U;
        }
        if (ret != ESP_OK) {
            break;
        }
        processed_entries += 1U;
        if ((processed_entries & UINT32_C(15)) == 0) {
            // taskYIELD() only offers the CPU to equal-priority tasks; block for one tick so IDLE can feed the task WDT.
            vTaskDelay(1);
        }
    }

    if (ret == ESP_OK && output.active) {
        on9kvdb_def::table_reference reference = {};
        ret = finish_compaction_output_unsafe(&output, &reference);
        if (ret == ESP_OK) {
            manifest.tables[output_start + output_count] = reference;
            output_count += 1U;
        }
    }
    if (ret != ESP_OK || output_count == 0 || greatest_sequence != next_transaction_sequence - 1U) {
        for (uint32_t slot = output_start; slot < output_start + bank_size; slot += 1) {
            manifest.tables[slot] = {};
        }
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    const size_t snapshot_offset = (sizeof(on9kvdb_def::manifest_record) + 15U) & ~static_cast<size_t>(15U);
    if (future_scratch_size < snapshot_offset + sizeof(on9kvdb_def::manifest_record)) {
        for (uint32_t slot = output_start; slot < output_start + bank_size; slot += 1) {
            manifest.tables[slot] = {};
        }
        return ESP_ERR_INVALID_SIZE;
    }
    // Output files are durable and validated, but remain unreachable until this manifest swap. Keep the previous in-memory
    // state outside write_manifest_copy()'s scratch range so an I/O failure can restore it without stack-heavy copies.
    auto *previous = new (future_scratch + snapshot_offset) on9kvdb_def::manifest_record(manifest);
    for (uint32_t slot = input_start; slot < input_start + bank_size; slot += 1) {
        manifest.tables[slot] = {};
    }
    // All output tables and copied chunks are already synchronized and unreachable. This manifest copy is the single point
    // at which they become live; until both copies stabilize, neither the old table bank nor old value bank may be recycled.
    manifest.active_table_bank = 1U - manifest.active_table_bank;
    manifest.active_value_bank = destination_value_bank;
    manifest.value_bank_generation[destination_value_bank] = destination_value_generation;
    manifest.value_bank_tail[destination_value_bank] = destination_value_tail;
    manifest.safe_checkpoint_sequence = greatest_sequence;
    manifest.next_table_generation += output_count;
    const uint32_t inactive_wal_slot = 1U - manifest.active_wal_slot;
    manifest.wal_generation[inactive_wal_slot] = 0;

    ret = sync_fd(storage_fds[descriptor_index(on9kvdb_def::file_kind::value_bank, destination_value_bank)]);
    if (ret == ESP_OK) {
        ret = write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_ready);
    }
    if (ret != ESP_OK) {
        manifest = *previous;
        for (uint32_t slot = output_start; slot < output_start + bank_size; slot += 1) {
            manifest.tables[slot] = {};
        }
        return ret;
    }

    reset_memtable_unsafe();
    // The first publication selects the new bank. Stabilization overwrites the remaining older manifest before either old
    // table files or the released WAL slot may be reused.
    manifest_stabilization_required = true;
    wal_tail[inactive_wal_slot] = on9kvdb_def::wal_record_region_offset;
    stats.wal_bytes_used = wal_tail[manifest.active_wal_slot] - on9kvdb_def::wal_record_region_offset;
    stats.logical_state_bytes = logical_state_bytes;
    stats.table_bytes_used = 0;
    uint64_t output_record_bytes = 0;
    for (uint32_t slot = output_start; slot < output_start + output_count; slot += 1) {
        const on9kvdb_def::table_reference &reference = manifest.tables[slot];
        output_record_bytes += reference.data_bytes;
        stats.table_bytes_used += on9kvdb_def::table_header_region_size +
                                  static_cast<uint64_t>(reference.data_block_count + 1U) * manifest.limits.sstable_block_bytes +
                                  on9kvdb_def::table_footer_slot_size;
    }
    ret = stabilize_manifest_unsafe();
    const int64_t compaction_end_us = esp_timer_get_time();
    const uint64_t elapsed_us =
        compaction_end_us >= compaction_start_us ? static_cast<uint64_t>(compaction_end_us - compaction_start_us) : 0;
    stats.compaction_count = saturating_add(stats.compaction_count, 1U);
    stats.compaction_input_record_bytes = saturating_add(stats.compaction_input_record_bytes, input_record_bytes);
    stats.compaction_output_record_bytes = saturating_add(stats.compaction_output_record_bytes, output_record_bytes);
    stats.last_compaction_time_us = elapsed_us;
    if (elapsed_us > stats.maximum_compaction_time_us) {
        stats.maximum_compaction_time_us = elapsed_us;
    }
    return ret;
}
