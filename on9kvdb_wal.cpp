#include <cstring>
#include <inttypes.h>

#include <esp_log.h>

#include "on9kvdb.hpp"

namespace
{
    static const constexpr uint32_t transaction_header_size = 8;
    static const constexpr uint32_t mutation_header_size = 8;
    static const constexpr uint32_t max_transaction_payload_size =
        CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE + transaction_header_size + on9kvdb_def::max_name_len +
        CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS * (mutation_header_size + on9kvdb_def::max_name_len);

    bool valid_value_shape(on9kvdb_type type, const uint8_t *value, uint32_t value_size)
    {
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
            return value != nullptr && value_size > 0 && value_size <= on9kvdb_def::max_value_len &&
                   value[value_size - 1U] == '\0';
        case on9kvdb_type::blob:
            return value_size <= on9kvdb_def::max_value_len;
        default:
            return false;
        }
    }
}

esp_err_t on9kvdb::ensure_wal_header(uint32_t slot, uint64_t generation, uint64_t first_transaction_sequence)
{
    if (slot >= on9kvdb_def::wal_file_count || generation == 0 || first_transaction_sequence == 0 ||
        manifest.geometry.wal_size > UINT32_MAX || io_frame == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    on9kvdb_def::wal_header header = {};
    header.database_id = manifest.database_id;
    header.generation = generation;
    header.first_transaction_sequence = first_transaction_sequence;
    header.slot = slot;
    header.record_region_start = on9kvdb_def::wal_record_region_offset;
    header.record_region_end = static_cast<uint32_t>(manifest.geometry.wal_size);
    header.frame_size = on9kvdb_def::wal_frame_size;
    header.state = on9kvdb_def::wal_header_state_active;

    const int wal_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::wal, slot)];
    for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::wal_header_slot_count; copy_slot += 1) {
        memset(io_frame, 0, on9kvdb_def::wal_header_slot_size);
        if (!on9kvdb_def::encode_wal_header(io_frame, on9kvdb_def::wal_header_slot_size, header)) {
            return ESP_ERR_INVALID_STATE;
        }

        const uint64_t offset =
            on9kvdb_def::wal_header_region_offset + static_cast<uint64_t>(copy_slot) * on9kvdb_def::wal_header_slot_size;
        esp_err_t ret = write_exact_fd(wal_fd, manifest.geometry.wal_size, offset, io_frame, on9kvdb_def::wal_header_slot_size);
        if (ret == ESP_OK) {
            ret = sync_fd(wal_fd);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t on9kvdb::initialise_first_wal()
{
    return ensure_wal_header(0, 1, 1);
}

esp_err_t on9kvdb::load_wal_header(uint32_t slot, uint64_t expected_generation, on9kvdb_def::wal_header *header_out) const
{
    if (slot >= on9kvdb_def::wal_file_count || expected_generation == 0 || header_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const int wal_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::wal, slot)];
    bool found = false;
    on9kvdb_def::wal_header selected = {};
    for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::wal_header_slot_count; copy_slot += 1) {
        uint8_t encoded[on9kvdb_def::wal_header_size] = {};
        const uint64_t offset =
            on9kvdb_def::wal_header_region_offset + static_cast<uint64_t>(copy_slot) * on9kvdb_def::wal_header_slot_size;
        esp_err_t ret = read_exact_fd(wal_fd, manifest.geometry.wal_size, offset, encoded, sizeof(encoded));
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::wal_header candidate = {};
        const on9kvdb_def::format_status status = on9kvdb_def::decode_wal_header(encoded, sizeof(encoded), &candidate);
        if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (status != on9kvdb_def::format_status::ok) {
            continue;
        }
        if (candidate.database_id != manifest.database_id || candidate.slot != slot ||
            candidate.generation != expected_generation || candidate.record_region_end != manifest.geometry.wal_size) {
            return ESP_ERR_INVALID_CRC;
        }
        if (found && !on9kvdb_def::wal_header_equal(selected, candidate)) {
            return ESP_ERR_INVALID_CRC;
        }
        selected = candidate;
        found = true;
    }

    if (!found) {
        return ESP_ERR_INVALID_CRC;
    }
    *header_out = selected;
    return ESP_OK;
}

esp_err_t on9kvdb::calculate_transaction_payload_unsafe(const transaction_slot &transaction_state, const handle_slot &handle,
                                                        uint32_t *payload_size_out, uint32_t *checksum_out) const
{
    if (payload_size_out == nullptr || checksum_out == nullptr || transaction_state.mutation_count == 0 ||
        transaction_state.mutation_count > CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t payload_size = transaction_header_size + handle.namespace_size;
    for (uint16_t idx = 0; idx < transaction_state.mutation_count; idx += 1) {
        const mutation_slot &mutation = transaction_state.mutations[idx];
        payload_size += mutation_header_size + mutation.key_size + mutation.value_size;
    }
    if (payload_size > max_transaction_payload_size || payload_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t crc = UINT32_MAX;
    uint32_t offset = 0;
    while (offset < payload_size) {
        const size_t chunk_size = payload_size - offset < on9kvdb_def::wal_frame_payload_capacity
                                      ? static_cast<size_t>(payload_size - offset)
                                      : on9kvdb_def::wal_frame_payload_capacity;
        esp_err_t ret = copy_transaction_payload_unsafe(transaction_state, handle, offset, future_scratch, chunk_size);
        if (ret != ESP_OK) {
            return ret;
        }
        crc = on9kvdb_def::calc_crc32_update(crc, future_scratch, chunk_size);
        offset += static_cast<uint32_t>(chunk_size);
    }

    *payload_size_out = static_cast<uint32_t>(payload_size);
    *checksum_out = ~crc;
    return ESP_OK;
}

void on9kvdb::copy_wal_payload_segment_unsafe(wal_payload_copy_state *copy_state, const uint8_t *data, size_t size)
{
    const uint64_t segment_end = static_cast<uint64_t>(copy_state->logical_offset) + size;
    const uint64_t request_start = copy_state->stream_offset;
    const uint64_t request_end = static_cast<uint64_t>(copy_state->stream_offset) + copy_state->request_size;
    if (segment_end > request_start && copy_state->logical_offset < request_end) {
        const size_t begin =
            request_start > copy_state->logical_offset ? static_cast<size_t>(request_start - copy_state->logical_offset) : 0;
        const size_t end = request_end < segment_end ? static_cast<size_t>(request_end - copy_state->logical_offset) : size;
        const size_t length = end - begin;
        memcpy(copy_state->destination + copy_state->copied, data + begin, length);
        copy_state->copied += length;
    }
    copy_state->logical_offset += static_cast<uint32_t>(size);
}

esp_err_t on9kvdb::copy_transaction_payload_unsafe(const transaction_slot &transaction_state, const handle_slot &handle,
                                                   uint32_t stream_offset, uint8_t *destination, size_t destination_size) const
{
    if (destination == nullptr && destination_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t total_size = transaction_header_size + handle.namespace_size;
    for (uint16_t idx = 0; idx < transaction_state.mutation_count; idx += 1) {
        total_size +=
            mutation_header_size + transaction_state.mutations[idx].key_size + transaction_state.mutations[idx].value_size;
    }
    if (stream_offset > total_size || destination_size > total_size - stream_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    wal_payload_copy_state copy_state = {};
    copy_state.stream_offset = stream_offset;
    copy_state.request_size = destination_size;
    copy_state.destination = destination;

    uint8_t transaction_header[transaction_header_size] = {};
    (void)on9kvdb_def::write_u16_le(transaction_header, sizeof(transaction_header), 0, handle.namespace_size);
    (void)on9kvdb_def::write_u16_le(transaction_header, sizeof(transaction_header), 2, transaction_state.mutation_count);
    copy_wal_payload_segment_unsafe(&copy_state, transaction_header, sizeof(transaction_header));
    copy_wal_payload_segment_unsafe(&copy_state, reinterpret_cast<const uint8_t *>(handle.namespace_name), handle.namespace_size);

    for (uint16_t idx = 0; idx < transaction_state.mutation_count; idx += 1) {
        const mutation_slot &mutation = transaction_state.mutations[idx];
        uint8_t mutation_header[mutation_header_size] = {};
        mutation_header[0] = mutation.kind;
        mutation_header[1] = mutation.type;
        (void)on9kvdb_def::write_u16_le(mutation_header, sizeof(mutation_header), 2, mutation.key_size);
        (void)on9kvdb_def::write_u32_le(mutation_header, sizeof(mutation_header), 4, mutation.value_size);
        copy_wal_payload_segment_unsafe(&copy_state, mutation_header, sizeof(mutation_header));
        copy_wal_payload_segment_unsafe(&copy_state, reinterpret_cast<const uint8_t *>(mutation.key), mutation.key_size);
        if (mutation.value_size > 0) {
            copy_wal_payload_segment_unsafe(&copy_state, transaction_staging + mutation.value_offset, mutation.value_size);
        }
    }

    return copy_state.copied == destination_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t on9kvdb::activate_second_wal_unsafe()
{
    if (manifest.active_wal_slot != 0 || manifest.wal_generation[0] == 0 || manifest.wal_generation[1] != 0 ||
        manifest.wal_generation[0] == UINT64_MAX || manifest.generation == UINT64_MAX) {
        return ESP_ERR_NO_MEM;
    }

    const uint64_t new_generation = manifest.wal_generation[0] + 1U;
    esp_err_t ret = ensure_wal_header(1, new_generation, next_transaction_sequence);
    if (ret != ESP_OK) {
        return ret;
    }

    const on9kvdb_def::manifest_record old_manifest = manifest;
    manifest.active_wal_slot = 1;
    manifest.wal_generation[1] = new_generation;
    ret = write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_ready);
    if (ret != ESP_OK) {
        manifest = old_manifest;
        return ret;
    }

    wal_tail[1] = on9kvdb_def::wal_record_region_offset;
    return ESP_OK;
}

esp_err_t on9kvdb::append_transaction_unsafe(transaction_slot *transaction_state, const handle_slot &handle)
{
    if (transaction_state == nullptr || next_transaction_sequence == 0 || next_transaction_sequence == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t payload_size = 0;
    uint32_t transaction_checksum = 0;
    esp_err_t ret = calculate_transaction_payload_unsafe(*transaction_state, handle, &payload_size, &transaction_checksum);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t frame_count =
        (payload_size + on9kvdb_def::wal_frame_payload_capacity - 1U) / on9kvdb_def::wal_frame_payload_capacity;
    const uint64_t transaction_bytes = static_cast<uint64_t>(frame_count) * on9kvdb_def::wal_frame_size;
    uint32_t active_slot = manifest.active_wal_slot;
    if (transaction_bytes > manifest.geometry.wal_size - wal_tail[active_slot]) {
        ret = activate_second_wal_unsafe();
        if (ret != ESP_OK) {
            return ret;
        }
        active_slot = manifest.active_wal_slot;
    }
    if (transaction_bytes > manifest.geometry.wal_size - wal_tail[active_slot]) {
        return ESP_ERR_NO_MEM;
    }

    const int wal_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::wal, active_slot)];
    uint32_t payload_offset = 0;
    for (uint32_t frame_index = 0; frame_index < frame_count; frame_index += 1) {
        const uint32_t frame_payload_size = payload_size - payload_offset < on9kvdb_def::wal_frame_payload_capacity
                                                ? payload_size - payload_offset
                                                : on9kvdb_def::wal_frame_payload_capacity;
        ret = copy_transaction_payload_unsafe(*transaction_state, handle, payload_offset, future_scratch, frame_payload_size);
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::wal_frame_header frame_header = {};
        frame_header.database_id = manifest.database_id;
        frame_header.wal_generation = manifest.wal_generation[active_slot];
        frame_header.transaction_sequence = next_transaction_sequence;
        frame_header.frame_index = static_cast<uint16_t>(frame_index);
        frame_header.frame_count = static_cast<uint16_t>(frame_count);
        frame_header.mutation_count = transaction_state->mutation_count;
        frame_header.flags = frame_index + 1U == frame_count ? on9kvdb_def::wal_frame_flag_commit : 0;
        frame_header.payload_size = frame_payload_size;
        frame_header.transaction_payload_size = payload_size;
        frame_header.transaction_checksum = transaction_checksum;
        if (!on9kvdb_def::encode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, frame_header, future_scratch,
                                           frame_payload_size)) {
            return ESP_ERR_INVALID_STATE;
        }

        ret = write_exact_fd(wal_fd, manifest.geometry.wal_size,
                             wal_tail[active_slot] + static_cast<uint64_t>(frame_index) * on9kvdb_def::wal_frame_size, io_frame,
                             on9kvdb_def::wal_frame_size);
        if (ret != ESP_OK) {
            return ret;
        }
        payload_offset += frame_payload_size;
    }

    ret = sync_fd(wal_fd);
    if (ret != ESP_OK) {
        return ret;
    }

    wal_tail[active_slot] += static_cast<uint32_t>(transaction_bytes);
    stats.wal_bytes_used += transaction_bytes;
    return ESP_OK;
}

esp_err_t on9kvdb::parse_recovered_transaction_unsafe(const uint8_t *payload, size_t payload_size,
                                                      uint16_t expected_mutation_count,
                                                      char namespace_name[on9kvdb_def::max_name_len + 1])
{
    if (payload == nullptr || payload_size < transaction_header_size || namespace_name == nullptr ||
        expected_mutation_count == 0 || expected_mutation_count > CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t namespace_size = 0;
    uint16_t mutation_count = 0;
    uint32_t reserved = 0;
    if (!on9kvdb_def::read_u16_le(payload, payload_size, 0, &namespace_size) ||
        !on9kvdb_def::read_u16_le(payload, payload_size, 2, &mutation_count) ||
        !on9kvdb_def::read_u32_le(payload, payload_size, 4, &reserved) || reserved != 0 ||
        mutation_count != expected_mutation_count || namespace_size == 0 || namespace_size > on9kvdb_def::max_name_len ||
        namespace_size > payload_size - transaction_header_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(namespace_name, payload + transaction_header_size, namespace_size);
    namespace_name[namespace_size] = '\0';
    if (!on9kvdb_def::validate_name(namespace_name)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *transaction = {};
    transaction->mutation_count = mutation_count;
    size_t offset = transaction_header_size + namespace_size;
    uint32_t staged_value_bytes = 0;
    for (uint16_t idx = 0; idx < mutation_count; idx += 1) {
        if (offset > payload_size || mutation_header_size > payload_size - offset) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        mutation_slot &mutation = transaction->mutations[idx];
        mutation = {};
        mutation.kind = payload[offset];
        mutation.type = payload[offset + 1U];
        uint16_t key_size = 0;
        uint32_t value_size = 0;
        if (!on9kvdb_def::read_u16_le(payload, payload_size, offset + 2U, &key_size) ||
            !on9kvdb_def::read_u32_le(payload, payload_size, offset + 4U, &value_size)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += mutation_header_size;
        if (key_size == 0 || key_size > on9kvdb_def::max_name_len || key_size > payload_size - offset) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        memcpy(mutation.key, payload + offset, key_size);
        mutation.key[key_size] = '\0';
        mutation.key_size = static_cast<uint8_t>(key_size);
        if (!on9kvdb_def::validate_name(mutation.key)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (uint16_t previous = 0; previous < idx; previous += 1) {
            if (strcmp(transaction->mutations[previous].key, mutation.key) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        offset += key_size;
        if (value_size > payload_size - offset || value_size > on9kvdb_def::max_value_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        mutation.value_offset = static_cast<uint32_t>(offset);
        mutation.value_size = value_size;
        const auto type = static_cast<on9kvdb_type>(mutation.type);
        if (mutation.kind == on9kvdb_def::mutation_kind_tombstone) {
            if (type != on9kvdb_type::any || value_size != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (mutation.kind != on9kvdb_def::mutation_kind_set || !valid_value_shape(type, payload + offset, value_size)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (value_size > CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE - staged_value_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        staged_value_bytes += value_size;
        offset += value_size;
    }

    if (offset != payload_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    transaction->staged_value_bytes = staged_value_bytes;
    return ESP_OK;
}

esp_err_t on9kvdb::scan_wal_slot(uint32_t slot, uint64_t generation, uint64_t *expected_sequence)
{
    if (slot >= on9kvdb_def::wal_file_count || generation == 0 || expected_sequence == nullptr || *expected_sequence == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    on9kvdb_def::wal_header wal_header = {};
    esp_err_t ret = load_wal_header(slot, generation, &wal_header);
    if (ret != ESP_OK) {
        return ret;
    }
    if (wal_header.first_transaction_sequence != *expected_sequence) {
        return ESP_ERR_INVALID_CRC;
    }

    const int wal_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::wal, slot)];
    uint32_t offset = wal_header.record_region_start;
    while (offset <= wal_header.record_region_end - on9kvdb_def::wal_frame_size) {
        ret = read_exact_fd(wal_fd, manifest.geometry.wal_size, offset, io_frame, on9kvdb_def::wal_frame_size);
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::wal_frame_header first = {};
        const uint8_t *first_payload = nullptr;
        const on9kvdb_def::format_status first_status =
            on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &first, &first_payload);
        if (first_status == on9kvdb_def::format_status::new_version) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (first_status != on9kvdb_def::format_status::ok) {
            bool later_valid = false;
            for (uint32_t later = offset + on9kvdb_def::wal_frame_size;
                 later <= wal_header.record_region_end - on9kvdb_def::wal_frame_size; later += on9kvdb_def::wal_frame_size) {
                ret = read_exact_fd(wal_fd, manifest.geometry.wal_size, later, io_frame, on9kvdb_def::wal_frame_size);
                if (ret != ESP_OK) {
                    return ret;
                }
                on9kvdb_def::wal_frame_header candidate = {};
                const uint8_t *candidate_payload = nullptr;
                if (on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &candidate, &candidate_payload) ==
                        on9kvdb_def::format_status::ok &&
                    candidate.database_id == manifest.database_id && candidate.wal_generation == generation &&
                    candidate.transaction_sequence >= *expected_sequence) {
                    later_valid = true;
                    break;
                }
            }
            if (later_valid) {
                return ESP_ERR_INVALID_CRC;
            }
            break;
        }

        if (first.database_id != manifest.database_id || first.wal_generation != generation ||
            first.transaction_sequence != *expected_sequence || first.frame_index != 0 || first.frame_count == 0 ||
            first.transaction_payload_size > max_transaction_payload_size ||
            static_cast<uint64_t>(first.frame_count) * on9kvdb_def::wal_frame_size > wal_header.record_region_end - offset) {
            return ESP_ERR_INVALID_CRC;
        }

        uint32_t payload_offset = 0;
        uint32_t transaction_crc = UINT32_MAX;
        for (uint16_t frame_index = 0; frame_index < first.frame_count; frame_index += 1) {
            ret = read_exact_fd(wal_fd, manifest.geometry.wal_size,
                                offset + static_cast<uint32_t>(frame_index) * on9kvdb_def::wal_frame_size, io_frame,
                                on9kvdb_def::wal_frame_size);
            if (ret != ESP_OK) {
                return ret;
            }

            on9kvdb_def::wal_frame_header frame = {};
            const uint8_t *payload_part = nullptr;
            const on9kvdb_def::format_status status =
                on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &frame, &payload_part);
            if (status != on9kvdb_def::format_status::ok || frame.database_id != first.database_id ||
                frame.wal_generation != first.wal_generation || frame.transaction_sequence != first.transaction_sequence ||
                frame.frame_index != frame_index || frame.frame_count != first.frame_count ||
                frame.mutation_count != first.mutation_count ||
                frame.transaction_payload_size != first.transaction_payload_size ||
                frame.transaction_checksum != first.transaction_checksum ||
                frame.payload_size > first.transaction_payload_size - payload_offset) {
                wal_tail[slot] = offset;
                return ESP_OK;
            }

            memcpy(transaction_staging + payload_offset, payload_part, frame.payload_size);
            transaction_crc = on9kvdb_def::calc_crc32_update(transaction_crc, payload_part, frame.payload_size);
            payload_offset += frame.payload_size;
        }

        if (payload_offset != first.transaction_payload_size || ~transaction_crc != first.transaction_checksum) {
            wal_tail[slot] = offset;
            return ESP_OK;
        }

        char namespace_name[on9kvdb_def::max_name_len + 1] = {};
        ret = parse_recovered_transaction_unsafe(transaction_staging, payload_offset, first.mutation_count, namespace_name);
        if (ret != ESP_OK) {
            return ret;
        }

        compact_memtable_unsafe();
        uint16_t namespace_index = 0;
        ret = preflight_memtable_transaction_unsafe(*transaction, namespace_name, &namespace_index);
        if (ret == ESP_OK) {
            ret = ensure_namespace_capacity_unsafe(namespace_name, &namespace_index, true);
        }
        if (ret != ESP_OK) {
            return ret;
        }
        apply_transaction_to_memtable_unsafe(*transaction, namespace_index, *expected_sequence);
        clear_transaction_unsafe();

        const uint32_t transaction_bytes = static_cast<uint32_t>(first.frame_count) * on9kvdb_def::wal_frame_size;
        offset += transaction_bytes;
        stats.wal_bytes_used += transaction_bytes;
        *expected_sequence += 1U;
        if (*expected_sequence == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    wal_tail[slot] = offset;
    return ESP_OK;
}

esp_err_t on9kvdb::recover_wal()
{
    if (manifest.state != on9kvdb_def::manifest_state_ready || manifest.wal_generation[0] == 0 ||
        manifest.active_wal_slot >= on9kvdb_def::wal_file_count) {
        return ESP_ERR_INVALID_CRC;
    }

    stats = {};
    namespace_count = 0;
    memtable_data_used = 0;
    memtable_entry_count = 0;
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MEMTABLE_ENTRY_COUNT; idx += 1) {
        memtable_index[idx] = {};
        memtable_index[idx].record_offset = UINT32_MAX;
    }

    uint64_t expected_sequence = 1;
    esp_err_t ret = scan_wal_slot(0, manifest.wal_generation[0], &expected_sequence);
    if (ret != ESP_OK) {
        return ret;
    }

    if (manifest.wal_generation[1] != 0) {
        if (manifest.active_wal_slot != 1 || manifest.wal_generation[1] <= manifest.wal_generation[0]) {
            return ESP_ERR_INVALID_CRC;
        }
        ret = scan_wal_slot(1, manifest.wal_generation[1], &expected_sequence);
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        if (manifest.active_wal_slot != 0) {
            return ESP_ERR_INVALID_CRC;
        }
        wal_tail[1] = on9kvdb_def::wal_record_region_offset;
    }

    next_transaction_sequence = expected_sequence;
    return ESP_OK;
}
