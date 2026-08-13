#include <cstring>
#include <inttypes.h>

#include <esp_log.h>

#include "on9kvdb.hpp"

#include <freertos/task.h>

namespace
{
    static const constexpr uint32_t transaction_header_size = 8;
    static const constexpr uint32_t mutation_header_size = 32;
    static const constexpr uint8_t mutation_flag_external_value = 1U << 0;
    static const constexpr uint32_t recovery_frame_delay_interval = 8;
    static const constexpr uint32_t recovery_transaction_delay_interval = 8;
    static const constexpr uint32_t max_transaction_payload_size =
        CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE + transaction_header_size + on9kvdb_def::max_name_len +
        CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS * (mutation_header_size + on9kvdb_def::max_name_len);

    uint32_t calc_mutation_checksum(const uint8_t header[mutation_header_size], const uint8_t *key, uint16_t key_size,
                                    const uint8_t *value, uint32_t value_size)
    {
        uint32_t crc = on9kvdb_def::calc_crc32_update(UINT32_MAX, header, 28);
        const uint8_t zeroes[sizeof(uint32_t)] = {};
        crc = on9kvdb_def::calc_crc32_update(crc, zeroes, sizeof(zeroes));
        crc = on9kvdb_def::calc_crc32_update(crc, key, key_size);
        crc = on9kvdb_def::calc_crc32_update(crc, value, value_size);
        return ~crc;
    }

    bool is_zero_bytes(const uint8_t *data, size_t size)
    {
        for (size_t index = 0; index < size; index += 1U) {
            if (data[index] != 0) {
                return false;
            }
        }
        return true;
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

    esp_err_t ret = ESP_OK;
    for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::wal_header_slot_count; copy_slot += 1) {
        memset(io_frame, 0, on9kvdb_def::wal_header_slot_size);
        if (!on9kvdb_def::encode_wal_header(io_frame, on9kvdb_def::wal_header_slot_size, header)) {
            return ESP_ERR_INVALID_STATE;
        }

        const uint64_t offset =
            on9kvdb_def::wal_header_region_offset + static_cast<uint64_t>(copy_slot) * on9kvdb_def::wal_header_slot_size;
        ret = write_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, slot, offset, io_frame, on9kvdb_def::wal_header_slot_size);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    // The generation remains unreachable until its manifest publication. Make both redundant headers durable together.
    return sync_storage_unsafe();
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

    bool found = false;
    on9kvdb_def::wal_header selected = {};
    for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::wal_header_slot_count; copy_slot += 1) {
        uint8_t encoded[on9kvdb_def::wal_header_size] = {};
        const uint64_t offset =
            on9kvdb_def::wal_header_region_offset + static_cast<uint64_t>(copy_slot) * on9kvdb_def::wal_header_slot_size;
        esp_err_t ret = read_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, slot, offset, encoded, sizeof(encoded));
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
        payload_size += mutation_header_size + mutation.key_size + (mutation.external_value ? 0 : mutation.value_size);
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
        total_size += mutation_header_size + transaction_state.mutations[idx].key_size +
                      (transaction_state.mutations[idx].external_value ? 0 : transaction_state.mutations[idx].value_size);
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
        mutation_header[1] = mutation.reserved0;
        (void)on9kvdb_def::write_u16_le(mutation_header, sizeof(mutation_header), 2, mutation.key_size);
        (void)on9kvdb_def::write_u32_le(mutation_header, sizeof(mutation_header), 4, mutation.value_size);
        if (mutation.external_value) {
            mutation_header[8] = mutation_flag_external_value;
            mutation_header[9] = mutation.external_value_ref.bank_slot;
            (void)on9kvdb_def::write_u64_le(mutation_header, sizeof(mutation_header), 12,
                                            mutation.external_value_ref.bank_generation);
            (void)on9kvdb_def::write_u32_le(mutation_header, sizeof(mutation_header), 20,
                                            mutation.external_value_ref.first_chunk_offset);
            (void)on9kvdb_def::write_u32_le(mutation_header, sizeof(mutation_header), 24,
                                            mutation.external_value_ref.value_checksum);
        }
        const uint8_t *inline_value = mutation.external_value ? nullptr : transaction_staging + mutation.value_offset;
        const uint32_t inline_value_size = mutation.external_value ? 0 : mutation.value_size;
        const uint32_t mutation_checksum =
            calc_mutation_checksum(mutation_header, mutation.key, mutation.key_size, inline_value, inline_value_size);
        (void)on9kvdb_def::write_u32_le(mutation_header, sizeof(mutation_header), 28, mutation_checksum);
        copy_wal_payload_segment_unsafe(&copy_state, mutation_header, sizeof(mutation_header));
        copy_wal_payload_segment_unsafe(&copy_state, mutation.key, mutation.key_size);
        if (inline_value_size > 0) {
            copy_wal_payload_segment_unsafe(&copy_state, inline_value, inline_value_size);
        }
    }

    return copy_state.copied == destination_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t on9kvdb::rotate_wal_unsafe()
{
    const uint32_t old_active_wal_slot = manifest.active_wal_slot;
    const uint32_t new_active_wal_slot = 1U - old_active_wal_slot;
    if (old_active_wal_slot >= on9kvdb_def::wal_file_count || manifest.wal_generation[old_active_wal_slot] == 0 ||
        manifest.wal_generation[new_active_wal_slot] != 0 || manifest.wal_generation[old_active_wal_slot] == UINT64_MAX ||
        manifest.generation > UINT64_MAX - 2U) {
        return ESP_ERR_NO_MEM;
    }

    // The target header may be overwritten only when both valid manifest copies already mark that slot unreferenced.
    esp_err_t ret = stabilize_manifest_unsafe();
    if (ret != ESP_OK) {
        return ret;
    }

    const uint64_t new_generation = manifest.wal_generation[old_active_wal_slot] + 1U;
    ret = ensure_wal_header(new_active_wal_slot, new_generation, next_transaction_sequence);
    if (ret != ESP_OK) {
        return ret;
    }

    manifest.active_wal_slot = new_active_wal_slot;
    manifest.wal_generation[new_active_wal_slot] = new_generation;
    ret = write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_ready);
    if (ret != ESP_OK) {
        manifest.active_wal_slot = old_active_wal_slot;
        manifest.wal_generation[new_active_wal_slot] = 0;
        return ret;
    }

    wal_tail[new_active_wal_slot] = on9kvdb_def::wal_record_region_offset;
    manifest_stabilization_required = true;
    return stabilize_manifest_unsafe();
}

esp_err_t on9kvdb::append_transaction_unsafe(transaction_slot *transaction_state, const handle_slot &handle)
{
    if (transaction_state == nullptr || next_transaction_sequence == 0 || next_transaction_sequence == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = stabilize_manifest_unsafe();
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t payload_size = 0;
    uint32_t transaction_checksum = 0;
    ret = calculate_transaction_payload_unsafe(*transaction_state, handle, &payload_size, &transaction_checksum);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t frame_count =
        (payload_size + on9kvdb_def::wal_frame_payload_capacity - 1U) / on9kvdb_def::wal_frame_payload_capacity;
    const uint64_t transaction_bytes = static_cast<uint64_t>(frame_count) * on9kvdb_def::wal_frame_size;
    uint32_t active_slot = manifest.active_wal_slot;
    bool staged_descriptors_may_have_moved = false;
    if (transaction_bytes > manifest.geometry.wal_size - wal_tail[active_slot]) {
        const uint32_t target_slot = 1U - active_slot;
        if (manifest.wal_generation[target_slot] != 0) {
            ret = compact_tables_unsafe();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG,
                         "WAL: compaction before rotation failed: ret=%s, sequence=%" PRIu64 ", active=%" PRIu32
                         ", target=%" PRIu32,
                         esp_err_to_name(ret), next_transaction_sequence, active_slot, target_slot);
                return ret;
            }
            staged_descriptors_may_have_moved = true;
        } else if (memtable_entry_count > 0) {
            // The current transaction is not included because its WAL record has not been written yet.
            ret = flush_memtable_unsafe();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "WAL: memtable flush before rotation failed: ret=%s, sequence=%" PRIu64, esp_err_to_name(ret),
                         next_transaction_sequence);
                return ret;
            }
            // A nominal flush may become a full compaction when the active
            // table bank has no free slot. Compaction relocates external
            // descriptors staged by the current transaction, so the WAL
            // checksum must be calculated from their final locations.
            staged_descriptors_may_have_moved = true;
        }
        ret = rotate_wal_unsafe();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "WAL: rotation failed: ret=%s, sequence=%" PRIu64, esp_err_to_name(ret), next_transaction_sequence);
            return ret;
        }
        active_slot = manifest.active_wal_slot;
    }
    if (transaction_bytes > manifest.geometry.wal_size - wal_tail[active_slot]) {
        return ESP_ERR_NO_MEM;
    }

    if (staged_descriptors_may_have_moved) {
        uint32_t recalculated_payload_size = 0;
        ret = calculate_transaction_payload_unsafe(*transaction_state, handle, &recalculated_payload_size, &transaction_checksum);
        if (ret != ESP_OK) {
            return ret;
        }
        if (recalculated_payload_size != payload_size) {
            return ESP_ERR_INVALID_STATE;
        }
    }

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

        ret = write_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, active_slot,
                                         wal_tail[active_slot] + static_cast<uint64_t>(frame_index) * on9kvdb_def::wal_frame_size,
                                         io_frame, on9kvdb_def::wal_frame_size);
        if (ret != ESP_OK) {
            return ret;
        }
        payload_offset += frame_payload_size;
    }

    ret = sync_storage_unsafe();
    if (ret != ESP_OK) {
        return ret;
    }

    wal_tail[active_slot] += static_cast<uint32_t>(transaction_bytes);
    stats.wal_bytes_used += transaction_bytes;
    return ESP_OK;
}

esp_err_t on9kvdb::parse_recovered_transaction_unsafe(const uint8_t *payload, size_t payload_size,
                                                      uint16_t expected_mutation_count,
                                                      uint8_t namespace_name[on9kvdb_def::max_name_len],
                                                      uint16_t *namespace_size_out)
{
    if (payload == nullptr || payload_size < transaction_header_size || namespace_name == nullptr ||
        namespace_size_out == nullptr || expected_mutation_count == 0 ||
        expected_mutation_count > CONFIG_ON9KVDB_MAX_TRANSACTION_MUTATIONS) {
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
    *namespace_size_out = namespace_size;

    *transaction = {};
    transaction->mutation_count = mutation_count;
    size_t offset = transaction_header_size + namespace_size;
    uint32_t staged_value_bytes = 0;
    for (uint16_t idx = 0; idx < mutation_count; idx += 1) {
        if (offset > payload_size || mutation_header_size > payload_size - offset) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const size_t mutation_start = offset;
        mutation_slot &mutation = transaction->mutations[idx];
        mutation = {};
        mutation.kind = payload[offset];
        mutation.reserved0 = payload[offset + 1U];
        uint16_t key_size = 0;
        uint32_t value_size = 0;
        uint32_t mutation_checksum = 0;
        if (!on9kvdb_def::read_u16_le(payload, payload_size, offset + 2U, &key_size) ||
            !on9kvdb_def::read_u32_le(payload, payload_size, offset + 4U, &value_size) ||
            !on9kvdb_def::read_u32_le(payload, payload_size, offset + 28U, &mutation_checksum)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t flags = payload[offset + 8U];
        const bool external = (flags & mutation_flag_external_value) != 0;
        if ((flags & ~mutation_flag_external_value) != 0 || payload[mutation_start + 10U] != 0 ||
            payload[mutation_start + 11U] != 0 || (!external && !is_zero_bytes(payload + mutation_start + 9U, 19U))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += mutation_header_size;
        if (key_size == 0 || key_size > on9kvdb_def::max_name_len || key_size > payload_size - offset) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        memcpy(mutation.key, payload + offset, key_size);
        mutation.key_size = key_size;
        for (uint16_t previous = 0; previous < idx; previous += 1) {
            if (transaction->mutations[previous].key_size == mutation.key_size &&
                memcmp(transaction->mutations[previous].key, mutation.key, mutation.key_size) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        offset += key_size;
        const uint32_t inline_value_size = external ? 0 : value_size;
        if (inline_value_size > payload_size - offset || (!external && value_size > on9kvdb_def::inline_value_len)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (calc_mutation_checksum(payload + mutation_start, mutation.key, key_size, payload + offset, inline_value_size) !=
            mutation_checksum) {
            return ESP_ERR_INVALID_CRC;
        }

        mutation.value_offset = static_cast<uint32_t>(offset);
        mutation.value_size = value_size;
        mutation.external_value = external;
        if (external) {
            mutation.external_value_ref = {};
            mutation.external_value_ref.bank_slot = payload[mutation_start + 9U];
            if (!on9kvdb_def::read_u64_le(payload, payload_size, mutation_start + 12U,
                                          &mutation.external_value_ref.bank_generation) ||
                !on9kvdb_def::read_u32_le(payload, payload_size, mutation_start + 20U,
                                          &mutation.external_value_ref.first_chunk_offset) ||
                !on9kvdb_def::read_u32_le(payload, payload_size, mutation_start + 24U,
                                          &mutation.external_value_ref.value_checksum)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            mutation.external_value_ref.value_size = value_size;
        }
        if (mutation.kind == on9kvdb_def::mutation_kind_tombstone) {
            if (mutation.reserved0 != 0 || value_size != 0 || external) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (mutation.kind != on9kvdb_def::mutation_kind_set || mutation.reserved0 != 0 ||
                   (external ? (value_size <= on9kvdb_def::inline_value_len ||
                                !on9kvdb_def::value_ref_is_valid(mutation.external_value_ref, manifest.geometry.value_bank_size))
                             : value_size > on9kvdb_def::inline_value_len)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (inline_value_size > CONFIG_ON9KVDB_TRANSACTION_STAGING_SIZE - staged_value_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        staged_value_bytes += inline_value_size;
        offset += inline_value_size;
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

    uint32_t offset = wal_header.record_region_start;
    uint32_t transactions_since_delay = 0;
    while (offset <= wal_header.record_region_end - on9kvdb_def::wal_frame_size) {
        ret = read_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, slot, offset, io_frame, on9kvdb_def::wal_frame_size);
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::wal_frame_header first = {};
        const uint8_t *first_payload = nullptr;
        const on9kvdb_def::format_status first_status =
            on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &first, &first_payload);
        if (first_status == on9kvdb_def::format_status::new_version ||
            first_status == on9kvdb_def::format_status::invalid_revision) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (first_status != on9kvdb_def::format_status::ok) {
            bool later_valid = false;
            ret = find_later_wal_frame_unsafe(slot, offset + on9kvdb_def::wal_frame_size, wal_header.record_region_end,
                                              generation, *expected_sequence, &later_valid);
            if (ret != ESP_OK) {
                return ret;
            }
            if (later_valid) {
                return ESP_ERR_INVALID_CRC;
            }
            break;
        }

        if (first.database_id != manifest.database_id || first.wal_generation != generation) {
            // Reusing a WAL slot overwrites committed frames from the beginning without erasing its remaining record region.
            // A valid frame from an older generation therefore marks the new generation's tail unless a later new-generation
            // frame proves that a hole exists.
            bool later_valid = false;
            ret = find_later_wal_frame_unsafe(slot, offset + on9kvdb_def::wal_frame_size, wal_header.record_region_end,
                                              generation, *expected_sequence, &later_valid);
            if (ret != ESP_OK) {
                return ret;
            }
            if (later_valid) {
                return ESP_ERR_INVALID_CRC;
            }
            break;
        }

        if (first.transaction_sequence != *expected_sequence || first.frame_index != 0 || first.frame_count == 0 ||
            first.transaction_payload_size > max_transaction_payload_size ||
            static_cast<uint64_t>(first.frame_count) * on9kvdb_def::wal_frame_size > wal_header.record_region_end - offset) {
            return ESP_ERR_INVALID_CRC;
        }

        uint32_t payload_offset = 0;
        uint32_t transaction_crc = UINT32_MAX;
        for (uint16_t frame_index = 0; frame_index < first.frame_count; frame_index += 1) {
            ret = read_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, slot,
                                            offset + static_cast<uint32_t>(frame_index) * on9kvdb_def::wal_frame_size, io_frame,
                                            on9kvdb_def::wal_frame_size);
            if (ret != ESP_OK) {
                return ret;
            }

            on9kvdb_def::wal_frame_header frame = {};
            const uint8_t *payload_part = nullptr;
            const on9kvdb_def::format_status status =
                on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &frame, &payload_part);
            if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
                return ESP_ERR_INVALID_VERSION;
            }
            if (status != on9kvdb_def::format_status::ok || frame.database_id != first.database_id ||
                frame.wal_generation != first.wal_generation || frame.transaction_sequence != first.transaction_sequence ||
                frame.frame_index != frame_index || frame.frame_count != first.frame_count ||
                frame.mutation_count != first.mutation_count ||
                frame.transaction_payload_size != first.transaction_payload_size ||
                frame.transaction_checksum != first.transaction_checksum ||
                frame.payload_size > first.transaction_payload_size - payload_offset) {
                bool later_valid = false;
                const uint32_t later_offset = offset + (static_cast<uint32_t>(frame_index) + 1U) * on9kvdb_def::wal_frame_size;
                ret = find_later_wal_frame_unsafe(slot, later_offset, wal_header.record_region_end, generation,
                                                  *expected_sequence, &later_valid);
                if (ret != ESP_OK) {
                    return ret;
                }
                if (later_valid) {
                    return ESP_ERR_INVALID_CRC;
                }
                wal_tail[slot] = offset;
                return ESP_OK;
            }

            memcpy(transaction_staging + payload_offset, payload_part, frame.payload_size);
            transaction_crc = on9kvdb_def::calc_crc32_update(transaction_crc, payload_part, frame.payload_size);
            payload_offset += frame.payload_size;
        }

        if (payload_offset != first.transaction_payload_size || ~transaction_crc != first.transaction_checksum) {
            bool later_valid = false;
            const uint32_t later_offset = offset + static_cast<uint32_t>(first.frame_count) * on9kvdb_def::wal_frame_size;
            ret = find_later_wal_frame_unsafe(slot, later_offset, wal_header.record_region_end, generation, *expected_sequence,
                                              &later_valid);
            if (ret != ESP_OK) {
                return ret;
            }
            if (later_valid) {
                return ESP_ERR_INVALID_CRC;
            }
            wal_tail[slot] = offset;
            return ESP_OK;
        }

        if (*expected_sequence > manifest.safe_checkpoint_sequence) {
            uint8_t namespace_name[on9kvdb_def::max_name_len] = {};
            uint16_t namespace_size = 0;
            ret = parse_recovered_transaction_unsafe(transaction_staging, payload_offset, first.mutation_count, namespace_name,
                                                     &namespace_size);
            if (ret != ESP_OK) {
                return ret;
            }

            for (uint16_t mutation_index = 0; mutation_index < transaction->mutation_count; mutation_index += 1U) {
                const mutation_slot &mutation = transaction->mutations[mutation_index];
                if (!mutation.external_value) {
                    continue;
                }
                const uint64_t chunk_count =
                    (static_cast<uint64_t>(mutation.external_value_ref.value_size) + on9kvdb_def::value_chunk_payload_size - 1U) /
                    on9kvdb_def::value_chunk_payload_size;
                const uint64_t end = static_cast<uint64_t>(mutation.external_value_ref.first_chunk_offset) +
                                     chunk_count * on9kvdb_def::value_chunk_size;
                const uint32_t bank_slot = mutation.external_value_ref.bank_slot;
                if (bank_slot != manifest.active_value_bank || end > manifest.geometry.value_bank_size ||
                    !value_ref_matches_bank_unsafe(mutation.external_value_ref, bank_slot,
                                                   manifest.value_bank_generation[bank_slot],
                                                   manifest.geometry.value_bank_size)) {
                    return ESP_ERR_INVALID_CRC;
                }
                if (end > manifest.value_bank_tail[bank_slot]) {
                    manifest.value_bank_tail[bank_slot] = static_cast<uint32_t>(end);
                }
            }

            compact_memtable_unsafe();
            uint16_t namespace_index = 0;
            const on9kvdb_bytes namespace_bytes = {namespace_name, namespace_size};
            ret = preflight_memtable_transaction_unsafe(*transaction, namespace_bytes, &namespace_index);
            if (ret == ESP_OK) {
                ret = ensure_namespace_capacity_unsafe(namespace_bytes, &namespace_index, true);
            }
            if (ret == ESP_OK) {
                ret = apply_transaction_to_memtable_unsafe(*transaction, namespace_index, *expected_sequence);
            }
            if (ret != ESP_OK) {
                return ret;
            }
            clear_transaction_unsafe();
        }

        const uint32_t transaction_bytes = static_cast<uint32_t>(first.frame_count) * on9kvdb_def::wal_frame_size;
        offset += transaction_bytes;
        stats.wal_bytes_used += transaction_bytes;
        *expected_sequence += 1U;
        if (*expected_sequence == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        transactions_since_delay += 1U;
        if (transactions_since_delay >= recovery_transaction_delay_interval) {
            vTaskDelay(1);
            transactions_since_delay = 0;
        }
    }

    wal_tail[slot] = offset;
    return ESP_OK;
}

esp_err_t on9kvdb::find_later_wal_frame_unsafe(uint32_t slot, uint32_t start_offset, uint32_t region_end, uint64_t generation,
                                               uint64_t minimum_sequence, bool *found_out)
{
    if (slot >= on9kvdb_def::wal_file_count || start_offset > region_end || generation == 0 || minimum_sequence == 0 ||
        found_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *found_out = false;
    uint32_t frames_since_delay = 0;
    for (uint32_t offset = start_offset; offset <= region_end - on9kvdb_def::wal_frame_size;
         offset += on9kvdb_def::wal_frame_size) {
        esp_err_t ret =
            read_storage_bytes_unsafe(on9kvdb_def::file_kind::wal, slot, offset, io_frame, on9kvdb_def::wal_frame_size);
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::wal_frame_header candidate = {};
        const uint8_t *candidate_payload = nullptr;
        const on9kvdb_def::format_status status =
            on9kvdb_def::decode_wal_frame(io_frame, on9kvdb_def::wal_frame_size, &candidate, &candidate_payload);
        if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (status == on9kvdb_def::format_status::ok && candidate.database_id == manifest.database_id &&
            candidate.wal_generation == generation && candidate.transaction_sequence >= minimum_sequence) {
            *found_out = true;
            break;
        }
        frames_since_delay += 1U;
        if (frames_since_delay >= recovery_frame_delay_interval) {
            vTaskDelay(1);
            frames_since_delay = 0;
        }
    }
    return ESP_OK;
}

esp_err_t on9kvdb::recover_wal()
{
    if (manifest.state != on9kvdb_def::manifest_state_ready || manifest.active_wal_slot >= on9kvdb_def::wal_file_count ||
        manifest.wal_generation[manifest.active_wal_slot] == 0) {
        return ESP_ERR_INVALID_CRC;
    }

    stats = {};
    namespace_count = 0;
    for (uint32_t idx = 0; idx < CONFIG_ON9KVDB_MAX_NAMESPACES; idx += 1) {
        namespaces[idx] = {};
    }
    reset_memtable_unsafe();
    esp_err_t ret = recover_tables_unsafe();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Recovery: SSTable recovery failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const uint32_t older_slot = 1U - manifest.active_wal_slot;
    uint32_t first_slot = manifest.wal_generation[older_slot] != 0 ? older_slot : manifest.active_wal_slot;
    if (manifest.wal_generation[older_slot] != 0 &&
        manifest.wal_generation[older_slot] >= manifest.wal_generation[manifest.active_wal_slot]) {
        ESP_LOGE(TAG, "Recovery: invalid WAL generation order");
        return ESP_ERR_INVALID_CRC;
    }

    on9kvdb_def::wal_header first_header = {};
    ret = load_wal_header(first_slot, manifest.wal_generation[first_slot], &first_header);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Recovery: WAL header failed: slot=%" PRIu32 ", ret=%s", first_slot, esp_err_to_name(ret));
        return ret;
    }
    if (manifest.safe_checkpoint_sequence == UINT64_MAX ||
        first_header.first_transaction_sequence > manifest.safe_checkpoint_sequence + 1U) {
        ESP_LOGE(TAG, "Recovery: WAL starts after checkpoint: first=%" PRIu64 ", checkpoint=%" PRIu64,
                 first_header.first_transaction_sequence, manifest.safe_checkpoint_sequence);
        return ESP_ERR_INVALID_CRC;
    }
    uint64_t expected_sequence = first_header.first_transaction_sequence;
    ret = scan_wal_slot(first_slot, manifest.wal_generation[first_slot], &expected_sequence);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Recovery: WAL scan failed: slot=%" PRIu32 ", ret=%s", first_slot, esp_err_to_name(ret));
        return ret;
    }
    if (first_slot != manifest.active_wal_slot) {
        ret = scan_wal_slot(manifest.active_wal_slot, manifest.wal_generation[manifest.active_wal_slot], &expected_sequence);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Recovery: active WAL scan failed: slot=%" PRIu32 ", ret=%s", manifest.active_wal_slot,
                     esp_err_to_name(ret));
            return ret;
        }
    } else {
        wal_tail[older_slot] = on9kvdb_def::wal_record_region_offset;
    }

    if (expected_sequence - 1U < manifest.safe_checkpoint_sequence) {
        ESP_LOGE(TAG, "Recovery: WAL ends before checkpoint: last=%" PRIu64 ", checkpoint=%" PRIu64, expected_sequence - 1U,
                 manifest.safe_checkpoint_sequence);
        return ESP_ERR_INVALID_CRC;
    }
    next_transaction_sequence = expected_sequence;
    stats.committed_transaction_count = expected_sequence - 1U;
    return stabilize_manifest_unsafe();
}
