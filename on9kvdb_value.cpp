#include <cstring>

#include "on9kvdb.hpp"

esp_err_t on9kvdb::get_value_reader_unsafe(on9kvdb_value_reader reader, value_reader_slot **slot_out)
{
    const value_reader_slot *slot = nullptr;
    const esp_err_t ret = static_cast<const on9kvdb *>(this)->get_value_reader_unsafe(reader, &slot);
    if (ret == ESP_OK) {
        *slot_out = const_cast<value_reader_slot *>(slot);
    }
    return ret;
}

esp_err_t on9kvdb::get_value_reader_unsafe(on9kvdb_value_reader reader, const value_reader_slot **slot_out) const
{
    if (slot_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t index = 0;
    uint32_t generation = 0;
    if (!on9kvdb_def::decode_handle_value(reader.raw, &index, &generation) || index >= CONFIG_ON9KVDB_MAX_VALUE_READERS ||
        value_readers == nullptr || !value_readers[index].used || value_readers[index].generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }
    *slot_out = &value_readers[index];
    return ESP_OK;
}

esp_err_t on9kvdb::get_value_writer_unsafe(on9kvdb_value_writer writer, value_writer_slot **slot_out)
{
    if (slot_out == nullptr || value_writer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t index = 0;
    uint32_t generation = 0;
    if (!on9kvdb_def::decode_handle_value(writer.raw, &index, &generation) || index != 0 || !value_writer->active ||
        value_writer->generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }
    *slot_out = value_writer;
    return ESP_OK;
}

bool on9kvdb::value_bank_is_pinned_unsafe(uint32_t bank_slot) const
{
    if (bank_slot >= on9kvdb_def::value_bank_count || value_readers == nullptr) {
        return false;
    }
    for (uint32_t index = 0; index < CONFIG_ON9KVDB_MAX_VALUE_READERS; index += 1U) {
        if (value_readers[index].used && value_readers[index].external &&
            value_readers[index].external_value.bank_slot == bank_slot) {
            return true;
        }
    }
    return false;
}

bool on9kvdb::value_bank_has_staged_reference_unsafe(uint32_t bank_slot) const
{
    if (bank_slot >= on9kvdb_def::value_bank_count || transaction == nullptr || !transaction->active) {
        return false;
    }
    for (uint16_t index = 0; index < transaction->mutation_count; index += 1U) {
        const mutation_slot &mutation = transaction->mutations[index];
        // A completed writer has a durable descriptor staged in this transaction but not yet in the memtable/table merge.
        // Treat it as a pin too: a foreground compaction may not overwrite the bank before the transaction commits or aborts.
        if (mutation.external_value && mutation.external_value_ref.bank_slot == bank_slot) {
            return true;
        }
    }
    return false;
}

esp_err_t on9kvdb::open_value_unsafe(const value_view &view, on9kvdb_value_reader *reader_out)
{
    if (reader_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view.tombstone) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((view.is_external && !on9kvdb_def::value_ref_is_valid(view.external_value, manifest.geometry.value_bank_size)) ||
        (!view.is_external && view.value_size != 0 && view.value == nullptr)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (value_readers == nullptr || value_reader_buffers == nullptr || handle_generation_counter == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t index = 0; index < CONFIG_ON9KVDB_MAX_VALUE_READERS; index += 1U) {
        value_reader_slot &reader = value_readers[index];
        if (reader.used) {
            continue;
        }
        const uint32_t generation = handle_generation_counter;
        handle_generation_counter = generation == on9kvdb_def::max_handle_generation ? 0 : handle_generation_counter + 1U;
        reader = {};
        reader.generation = generation;
        reader.value_size = view.value_size;
        reader.used = true;
        reader.external = view.is_external;
        reader.external_value = view.external_value;
        if (!view.is_external && view.value_size > 0) {
            memcpy(value_reader_buffers + static_cast<size_t>(index) * on9kvdb_def::value_chunk_size, view.value,
                   view.value_size);
            reader.buffer_size = view.value_size;
        }
        *reader_out = on9kvdb_value_reader(on9kvdb_def::make_handle_value(static_cast<uint16_t>(index), generation));
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t on9kvdb::fill_reader_buffer_unsafe(value_reader_slot *reader)
{
    if (reader == nullptr || !reader->used || !reader->external || reader->cursor >= reader->value_size ||
        !on9kvdb_def::value_ref_is_valid(reader->external_value, manifest.geometry.value_bank_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t chunk_index = reader->cursor / on9kvdb_def::value_chunk_payload_size;
    const uint32_t payload_offset = reader->cursor % on9kvdb_def::value_chunk_payload_size;
    const uint64_t chunk_offset = static_cast<uint64_t>(reader->external_value.first_chunk_offset) +
                                  static_cast<uint64_t>(chunk_index) * on9kvdb_def::value_chunk_size;
    if (chunk_offset > manifest.geometry.value_bank_size - on9kvdb_def::value_chunk_size) {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t reader_index = CONFIG_ON9KVDB_MAX_VALUE_READERS;
    for (uint32_t index = 0; index < CONFIG_ON9KVDB_MAX_VALUE_READERS; index += 1U) {
        if (&value_readers[index] == reader) {
            reader_index = index;
            break;
        }
    }
    if (reader_index == CONFIG_ON9KVDB_MAX_VALUE_READERS) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *buffer = value_reader_buffers + static_cast<size_t>(reader_index) * on9kvdb_def::value_chunk_size;
    const int file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::value_bank, reader->external_value.bank_slot)];
    esp_err_t ret =
        read_exact_fd(file_fd, manifest.geometry.value_bank_size, chunk_offset, buffer, on9kvdb_def::value_chunk_size);
    if (ret != ESP_OK) {
        return ret;
    }
    on9kvdb_def::value_chunk_header header = {};
    const uint8_t *payload = nullptr;
    const on9kvdb_def::format_status status =
        on9kvdb_def::decode_value_chunk(buffer, on9kvdb_def::value_chunk_size, &header, &payload);
    if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (status != on9kvdb_def::format_status::ok || header.database_id != manifest.database_id ||
        header.bank_generation != reader->external_value.bank_generation ||
        header.first_chunk_offset != reader->external_value.first_chunk_offset || header.value_size != reader->value_size ||
        header.value_offset != chunk_index * on9kvdb_def::value_chunk_payload_size || payload_offset >= header.payload_size) {
        return ESP_ERR_INVALID_CRC;
    }
    const uint32_t available = header.payload_size - payload_offset;
    memmove(buffer, payload + payload_offset, available);
    reader->buffer_value_offset = reader->cursor;
    reader->buffer_size = available;
    return ESP_OK;
}

esp_err_t on9kvdb::open_value(on9kvdb_handle handle, on9kvdb_bytes key, on9kvdb_value_reader *reader_out)
{
    if (!on9kvdb_def::validate_bytes(key.data, key.size) || reader_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    const handle_slot *handle_state = nullptr;
    ret = get_handle_slot_unsafe(handle, &handle_state);
    value_view view = {};
    ret = ret ?: lookup_committed_unsafe({handle_state->namespace_name, handle_state->namespace_size}, key, &view);
    ret = ret ?: open_value_unsafe(view, reader_out);
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::get_value_size(on9kvdb_value_reader reader, uint32_t *size_out) const
{
    if (size_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    const value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    if (ret == ESP_OK) {
        *size_out = reader_state->value_size;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::peek_value(on9kvdb_value_reader reader, const uint8_t **data_out, uint32_t *size_out)
{
    if (data_out == nullptr || size_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *data_out = nullptr;
    *size_out = 0;
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    if (ret == ESP_OK && reader_state->cursor < reader_state->value_size &&
        (reader_state->cursor < reader_state->buffer_value_offset ||
         reader_state->cursor >= reader_state->buffer_value_offset + reader_state->buffer_size)) {
        ret = reader_state->external ? fill_reader_buffer_unsafe(reader_state) : ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && reader_state->cursor < reader_state->value_size) {
        uint32_t reader_index = CONFIG_ON9KVDB_MAX_VALUE_READERS;
        for (uint32_t index = 0; index < CONFIG_ON9KVDB_MAX_VALUE_READERS; index += 1U) {
            if (&value_readers[index] == reader_state) {
                reader_index = index;
                break;
            }
        }
        if (reader_index == CONFIG_ON9KVDB_MAX_VALUE_READERS) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            const uint32_t buffer_offset = reader_state->cursor - reader_state->buffer_value_offset;
            *data_out = value_reader_buffers + static_cast<size_t>(reader_index) * on9kvdb_def::value_chunk_size + buffer_offset;
            *size_out = reader_state->buffer_size - buffer_offset;
        }
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::consume_value(on9kvdb_value_reader reader, uint32_t size)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    if (ret == ESP_OK && size > reader_state->value_size - reader_state->cursor) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) {
        reader_state->cursor += size;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::read_value_into(on9kvdb_value_reader reader, void *destination, uint32_t destination_size,
                                   uint32_t *read_size_out)
{
    if ((destination == nullptr && destination_size != 0) || read_size_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *read_size_out = 0;
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    uint8_t *output = static_cast<uint8_t *>(destination);
    while (ret == ESP_OK && *read_size_out < destination_size && reader_state->cursor < reader_state->value_size) {
        if (reader_state->cursor < reader_state->buffer_value_offset ||
            reader_state->cursor >= reader_state->buffer_value_offset + reader_state->buffer_size) {
            ret = reader_state->external ? fill_reader_buffer_unsafe(reader_state) : ESP_ERR_INVALID_STATE;
            if (ret != ESP_OK) {
                break;
            }
        }
        uint32_t reader_index = CONFIG_ON9KVDB_MAX_VALUE_READERS;
        for (uint32_t index = 0; index < CONFIG_ON9KVDB_MAX_VALUE_READERS; index += 1U) {
            if (&value_readers[index] == reader_state) {
                reader_index = index;
                break;
            }
        }
        if (reader_index == CONFIG_ON9KVDB_MAX_VALUE_READERS) {
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        const uint32_t buffer_offset = reader_state->cursor - reader_state->buffer_value_offset;
        uint32_t copy_size = reader_state->buffer_size - buffer_offset;
        const uint32_t destination_remaining = destination_size - *read_size_out;
        if (copy_size > destination_remaining) {
            copy_size = destination_remaining;
        }
        memcpy(output + *read_size_out,
               value_reader_buffers + static_cast<size_t>(reader_index) * on9kvdb_def::value_chunk_size + buffer_offset,
               copy_size);
        *read_size_out += copy_size;
        reader_state->cursor += copy_size;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::seek_value(on9kvdb_value_reader reader, uint32_t offset)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    if (ret == ESP_OK && offset > reader_state->value_size) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) {
        reader_state->cursor = offset;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::close(on9kvdb_value_reader reader)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    value_reader_slot *reader_state = nullptr;
    ret = get_value_reader_unsafe(reader, &reader_state);
    if (ret == ESP_OK) {
        const uint32_t generation = reader_state->generation;
        *reader_state = {};
        reader_state->generation = generation;
    }
    release_operation_lock();
    return ret;
}

esp_err_t on9kvdb::begin_value_write_unsafe(on9kvdb_transaction_handle transaction_handle, on9kvdb_bytes key, uint32_t value_size,
                                            on9kvdb_value_writer *writer_out)
{
    if (!on9kvdb_def::validate_bytes(key.data, key.size) || writer_out == nullptr || value_size > on9kvdb_def::max_value_len ||
        value_writer == nullptr || io_frame == nullptr || value_writer_buffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    transaction_slot *transaction_state = nullptr;
    esp_err_t ret = get_transaction_unsafe(transaction_handle, &transaction_state);
    if (ret != ESP_OK) {
        return ret;
    }
    if (value_writer->active || handle_generation_counter == 0 || manifest.active_value_bank >= on9kvdb_def::value_bank_count) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool inline_value = value_size <= on9kvdb_def::inline_value_len;
    uint32_t bank_slot = 0;
    uint32_t tail = 0;
    if (!inline_value) {
        // Value-bank chunks are append-only. If the active bank cannot hold the complete stream, compact first; compaction
        // writes the current live set into the other bank and publishes it atomically through the manifest before reuse.
        const uint64_t chunk_count = (static_cast<uint64_t>(value_size) + on9kvdb_def::value_chunk_payload_size - 1U) /
                                     on9kvdb_def::value_chunk_payload_size;
        const uint64_t required_bytes = chunk_count * on9kvdb_def::value_chunk_size;
        bank_slot = manifest.active_value_bank;
        tail = manifest.value_bank_tail[bank_slot];
        if (tail < on9kvdb_def::identity_region_size || tail > manifest.geometry.value_bank_size ||
            required_bytes > manifest.geometry.value_bank_size - tail) {
            // Reclaim obsolete large values with a full table/value-bank compaction before refusing a writer.  This has no
            // per-write allocation; it only uses the fixed compaction and I/O frames reserved at init.
            ret = compact_tables_unsafe();
            if (ret != ESP_OK) {
                return ret;
            }
            bank_slot = manifest.active_value_bank;
            tail = manifest.value_bank_tail[bank_slot];
            if (tail < on9kvdb_def::identity_region_size || tail > manifest.geometry.value_bank_size ||
                required_bytes > manifest.geometry.value_bank_size - tail) {
                return ESP_ERR_NO_MEM;
            }
        }
    }
    const uint32_t generation = handle_generation_counter;
    handle_generation_counter = generation == on9kvdb_def::max_handle_generation ? 0 : handle_generation_counter + 1U;
    *value_writer = {};
    value_writer->generation = generation;
    value_writer->transaction_handle_raw = transaction_handle.raw;
    value_writer->expected_size = value_size;
    value_writer->next_chunk_offset = tail;
    value_writer->key_size = key.size;
    value_writer->inline_value = inline_value;
    value_writer->reference.bank_slot = static_cast<uint8_t>(bank_slot);
    if (!inline_value) {
        value_writer->reference.bank_generation = manifest.value_bank_generation[bank_slot];
        value_writer->reference.first_chunk_offset = tail;
    }
    value_writer->reference.value_size = value_size;
    value_writer->active = true;
    memcpy(value_writer->key, key.data, key.size);
    memset(value_writer_buffer, 0, on9kvdb_def::value_chunk_size);
    *writer_out = on9kvdb_value_writer(on9kvdb_def::make_handle_value(0, generation));
    return ESP_OK;
}

esp_err_t on9kvdb::flush_value_writer_chunk_unsafe(value_writer_slot *writer, bool final_chunk)
{
    if (writer == nullptr || !writer->active || writer->chunk_payload_size == 0 ||
        writer->next_chunk_offset > manifest.geometry.value_bank_size - on9kvdb_def::value_chunk_size ||
        final_chunk != (writer->written_size == writer->expected_size)) {
        return ESP_ERR_INVALID_STATE;
    }
    on9kvdb_def::value_chunk_header header = {};
    header.database_id = manifest.database_id;
    header.bank_generation = writer->reference.bank_generation;
    header.first_chunk_offset = writer->reference.first_chunk_offset;
    header.value_size = writer->expected_size;
    header.value_offset = writer->written_size - writer->chunk_payload_size;
    header.payload_size = static_cast<uint16_t>(writer->chunk_payload_size);
    header.flags = final_chunk ? on9kvdb_def::value_chunk_flag_final : 0;
    if (value_writer_buffer == nullptr ||
        !on9kvdb_def::encode_value_chunk(io_frame, on9kvdb_def::value_chunk_size, header, value_writer_buffer)) {
        return ESP_ERR_INVALID_STATE;
    }
    const int file_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::value_bank, writer->reference.bank_slot)];
    esp_err_t ret = write_exact_fd(file_fd, manifest.geometry.value_bank_size, writer->next_chunk_offset, io_frame,
                                   on9kvdb_def::value_chunk_size);
    if (ret != ESP_OK) {
        return ret;
    }
    writer->next_chunk_offset += on9kvdb_def::value_chunk_size;
    writer->chunk_payload_size = 0;
    memset(value_writer_buffer, 0, on9kvdb_def::value_chunk_size);
    return ESP_OK;
}

esp_err_t on9kvdb::copy_external_value_unsafe(const on9kvdb_def::value_ref &source, uint32_t destination_bank,
                                              uint64_t destination_generation, uint32_t *destination_tail,
                                              on9kvdb_def::value_ref *destination_out)
{
    if (!on9kvdb_def::value_ref_is_valid(source, manifest.geometry.value_bank_size) ||
        destination_bank >= on9kvdb_def::value_bank_count || destination_generation == 0 || destination_tail == nullptr ||
        destination_out == nullptr || io_frame == nullptr || value_writer_buffer == nullptr ||
        source.bank_slot == destination_bank || *destination_tail < on9kvdb_def::identity_region_size ||
        *destination_tail > manifest.geometry.value_bank_size - on9kvdb_def::value_chunk_size ||
        *destination_tail % on9kvdb_def::value_chunk_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t chunk_count = (static_cast<uint64_t>(source.value_size) + on9kvdb_def::value_chunk_payload_size - 1U) /
                                 on9kvdb_def::value_chunk_payload_size;
    const uint64_t required_bytes = chunk_count * on9kvdb_def::value_chunk_size;
    if (required_bytes > manifest.geometry.value_bank_size - *destination_tail) {
        return ESP_ERR_NO_MEM;
    }

    on9kvdb_def::value_ref destination = source;
    destination.bank_slot = static_cast<uint8_t>(destination_bank);
    destination.bank_generation = destination_generation;
    destination.first_chunk_offset = *destination_tail;

    const int source_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::value_bank, source.bank_slot)];
    const int destination_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::value_bank, destination_bank)];
    uint32_t checksum_state = UINT32_MAX;
    uint32_t output_offset = destination.first_chunk_offset;
    for (uint64_t index = 0; index < chunk_count; index += 1U) {
        const uint64_t input_offset = static_cast<uint64_t>(source.first_chunk_offset) + index * on9kvdb_def::value_chunk_size;
        esp_err_t ret =
            read_exact_fd(source_fd, manifest.geometry.value_bank_size, input_offset, io_frame, on9kvdb_def::value_chunk_size);
        if (ret != ESP_OK) {
            return ret;
        }
        on9kvdb_def::value_chunk_header input_header = {};
        const uint8_t *payload = nullptr;
        const on9kvdb_def::format_status status =
            on9kvdb_def::decode_value_chunk(io_frame, on9kvdb_def::value_chunk_size, &input_header, &payload);
        if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
            return ESP_ERR_INVALID_VERSION;
        }
        const uint32_t logical_offset = static_cast<uint32_t>(index * on9kvdb_def::value_chunk_payload_size);
        const uint32_t expected_payload = source.value_size - logical_offset < on9kvdb_def::value_chunk_payload_size
                                              ? source.value_size - logical_offset
                                              : on9kvdb_def::value_chunk_payload_size;
        const bool final = index + 1U == chunk_count;
        if (status != on9kvdb_def::format_status::ok || input_header.database_id != manifest.database_id ||
            input_header.bank_generation != source.bank_generation ||
            input_header.first_chunk_offset != source.first_chunk_offset || input_header.value_size != source.value_size ||
            input_header.value_offset != logical_offset || input_header.payload_size != expected_payload ||
            final != ((input_header.flags & on9kvdb_def::value_chunk_flag_final) != 0)) {
            return ESP_ERR_INVALID_CRC;
        }
        // Decoding verifies the chunk record and payload CRCs. The rolling checksum additionally proves that the descriptor's
        // whole-value CRC still covers this exact ordered chunk sequence before a copied descriptor can be published.
        checksum_state = on9kvdb_def::calc_crc32_update(checksum_state, payload, expected_payload);

        on9kvdb_def::value_chunk_header output_header = input_header;
        output_header.bank_generation = destination_generation;
        output_header.first_chunk_offset = destination.first_chunk_offset;
        if (!on9kvdb_def::encode_value_chunk(value_writer_buffer, on9kvdb_def::value_chunk_size, output_header, payload)) {
            return ESP_ERR_INVALID_STATE;
        }
        ret = write_exact_fd(destination_fd, manifest.geometry.value_bank_size, output_offset, value_writer_buffer,
                             on9kvdb_def::value_chunk_size);
        if (ret != ESP_OK) {
            return ret;
        }
        output_offset += on9kvdb_def::value_chunk_size;
    }
    if (~checksum_state != source.value_checksum) {
        return ESP_ERR_INVALID_CRC;
    }
    *destination_tail = output_offset;
    *destination_out = destination;
    return ESP_OK;
}

esp_err_t on9kvdb::write_value_unsafe(value_writer_slot *writer, const uint8_t *data, uint32_t size)
{
    if (writer == nullptr || !writer->active || (data == nullptr && size != 0) ||
        size > writer->expected_size - writer->written_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->inline_value) {
        // Inline writer data never reaches a value bank. It is staged directly for the WAL/table record at finish(), exactly
        // like set(), while still giving callers one progressive writer contract for every value size.
        if (size > 0) {
            memcpy(value_writer_buffer + writer->written_size, data, size);
            writer->checksum_state = on9kvdb_def::calc_crc32_update(static_cast<uint32_t>(writer->checksum_state), data, size);
        }
        writer->written_size += size;
        return ESP_OK;
    }
    uint32_t copied = 0;
    while (copied < size) {
        const uint32_t available = on9kvdb_def::value_chunk_payload_size - writer->chunk_payload_size;
        uint32_t part = size - copied;
        if (part > available) {
            part = available;
        }
        memcpy(value_writer_buffer + writer->chunk_payload_size, data + copied, part);
        writer->checksum_state =
            on9kvdb_def::calc_crc32_update(static_cast<uint32_t>(writer->checksum_state), data + copied, part);
        writer->chunk_payload_size += part;
        writer->written_size += part;
        copied += part;
        if (writer->chunk_payload_size == on9kvdb_def::value_chunk_payload_size ||
            writer->written_size == writer->expected_size) {
            const esp_err_t ret = flush_value_writer_chunk_unsafe(writer, writer->written_size == writer->expected_size);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    return ESP_OK;
}

esp_err_t on9kvdb::finish_value_write_unsafe(value_writer_slot *writer)
{
    if (writer == nullptr || !writer->active || writer->written_size != writer->expected_size ||
        writer->chunk_payload_size != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const on9kvdb_bytes key = {writer->key, writer->key_size};
    const on9kvdb_transaction_handle transaction_handle(writer->transaction_handle_raw);
    esp_err_t ret = ESP_OK;
    if (writer->inline_value) {
        ret = stage_value_unsafe(transaction_handle, key, value_writer_buffer, writer->expected_size,
                                 on9kvdb_def::mutation_kind_set);
    } else {
        writer->reference.value_checksum = ~static_cast<uint32_t>(writer->checksum_state);
        ret = stage_external_value_unsafe(transaction_handle, key, writer->reference);
    }
    if (ret == ESP_OK && !writer->inline_value) {
        // Commit synchronizes every dirty bank before appending a WAL record that can reference it. Updating this in-memory
        // tail here merely reserves the append range; the manifest receives the durable tail during the next publication.
        manifest.value_bank_tail[writer->reference.bank_slot] = writer->next_chunk_offset;
        value_bank_dirty_mask |= static_cast<uint8_t>(UINT8_C(1) << writer->reference.bank_slot);
    }
    const uint32_t generation = writer->generation;
    *writer = {};
    writer->generation = generation;
    return ret;
}

esp_err_t on9kvdb::begin_value_write(on9kvdb_transaction_handle transaction, on9kvdb_bytes key, uint32_t value_size,
                                     on9kvdb_value_writer *writer_out)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret == ESP_OK) {
        ret = begin_value_write_unsafe(transaction, key, value_size, writer_out);
        release_operation_lock();
    }
    return ret;
}

esp_err_t on9kvdb::write_value(on9kvdb_value_writer writer, const uint8_t *data, uint32_t size)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret == ESP_OK) {
        value_writer_slot *writer_state = nullptr;
        ret = get_value_writer_unsafe(writer, &writer_state);
        ret = ret ?: write_value_unsafe(writer_state, data, size);
        release_operation_lock();
    }
    return ret;
}

esp_err_t on9kvdb::finish_value_write(on9kvdb_value_writer writer)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret == ESP_OK) {
        value_writer_slot *writer_state = nullptr;
        ret = get_value_writer_unsafe(writer, &writer_state);
        ret = ret ?: finish_value_write_unsafe(writer_state);
        release_operation_lock();
    }
    return ret;
}

esp_err_t on9kvdb::abort_value_write(on9kvdb_value_writer writer)
{
    esp_err_t ret = acquire_operation_lock();
    if (ret == ESP_OK) {
        value_writer_slot *writer_state = nullptr;
        ret = get_value_writer_unsafe(writer, &writer_state);
        if (ret == ESP_OK) {
            const uint32_t generation = writer_state->generation;
            *writer_state = {};
            writer_state->generation = generation;
        }
        release_operation_lock();
    }
    return ret;
}
