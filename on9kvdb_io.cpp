#include <cstring>

#include "on9kvdb.hpp"

#include <freertos/task.h>

esp_err_t on9kvdb::get_storage_region_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t *offset_out,
                                             uint64_t *size_out) const
{
    if (offset_out == nullptr || size_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // The physical image has no allocation metadata: this arithmetic is the
    // format's authoritative manifest -> WAL -> table -> value-bank mapping.
    // During first provisioning no manifest exists yet, so use the exact
    // build geometry; after open, use the geometry validated from the image.
    const on9kvdb_def::storage_geometry &geometry =
        manifest.geometry.provisioned_size != 0 ? manifest.geometry : get_build_geometry();
    uint64_t offset = 0;
    uint64_t size = 0;
    if (kind == on9kvdb_def::file_kind::manifest) {
        if (slot != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        size = geometry.manifest_size;
    } else if (kind == on9kvdb_def::file_kind::wal) {
        if (slot >= geometry.wal_count || !on9kvdb_def::checked_mul_u64(slot, geometry.wal_size, &offset) ||
            !on9kvdb_def::checked_add_u64(geometry.manifest_size, offset, &offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
        size = geometry.wal_size;
    } else if (kind == on9kvdb_def::file_kind::table) {
        uint64_t wal_bytes = 0;
        uint64_t table_offset = 0;
        if (slot >= geometry.table_count || !on9kvdb_def::checked_mul_u64(geometry.wal_count, geometry.wal_size, &wal_bytes) ||
            !on9kvdb_def::checked_mul_u64(slot, geometry.table_size, &table_offset) ||
            !on9kvdb_def::checked_add_u64(geometry.manifest_size, wal_bytes, &offset) ||
            !on9kvdb_def::checked_add_u64(offset, table_offset, &offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
        size = geometry.table_size;
    } else if (kind == on9kvdb_def::file_kind::value_bank) {
        uint64_t wal_bytes = 0;
        uint64_t table_bytes = 0;
        uint64_t value_offset = 0;
        if (slot >= geometry.value_bank_count ||
            !on9kvdb_def::checked_mul_u64(geometry.wal_count, geometry.wal_size, &wal_bytes) ||
            !on9kvdb_def::checked_mul_u64(geometry.table_count, geometry.table_size, &table_bytes) ||
            !on9kvdb_def::checked_mul_u64(slot, geometry.value_bank_size, &value_offset) ||
            !on9kvdb_def::checked_add_u64(geometry.manifest_size, wal_bytes, &offset) ||
            !on9kvdb_def::checked_add_u64(offset, table_bytes, &offset) ||
            !on9kvdb_def::checked_add_u64(offset, value_offset, &offset)) {
            return ESP_ERR_INVALID_SIZE;
        }
        size = geometry.value_bank_size;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t region_end = 0;
    if (!on9kvdb_def::checked_add_u64(offset, size, &region_end) || region_end > geometry.provisioned_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *offset_out = offset;
    *size_out = size;
    return ESP_OK;
}

esp_err_t on9kvdb::read_storage_bytes_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t offset, void *destination,
                                             size_t size) const
{
    if ((destination == nullptr && size != 0) || storage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t region_offset = 0;
    uint64_t region_size = 0;
    esp_err_t ret = get_storage_region_unsafe(kind, slot, &region_offset, &region_size);
    if (ret != ESP_OK || offset > region_size || size > region_size - offset) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    if (size == 0) {
        return ESP_OK;
    }

    const uint32_t block_size = storage->block_size();
    if (block_size == 0 || on9kvdb_def::format_alignment % block_size != 0 || block_size > on9kvdb_def::format_alignment ||
        io_bounce == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t absolute_offset = 0;
    if (!on9kvdb_def::checked_add_u64(region_offset, offset, &absolute_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (absolute_offset % block_size == 0 && size % block_size == 0) {
        // The common table/WAL/value path is already device-block aligned and
        // can DMA directly into the caller's fixed buffer without a copy.
        const uint64_t block_count = size / block_size;
        if (block_count > UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        return storage->read_blocks(absolute_offset / block_size, destination, static_cast<uint32_t>(block_count));
    }

    // Metadata decoders often request a small record prefix. Read each native
    // block into the permanent bounce frame rather than making callers round
    // up their output buffers or allocating a temporary buffer per request.
    uint8_t *output = static_cast<uint8_t *>(destination);
    size_t remaining = size;
    while (remaining > 0) {
        const uint32_t block_offset = static_cast<uint32_t>(absolute_offset % block_size);
        const size_t copy_size = remaining < block_size - block_offset ? remaining : block_size - block_offset;
        ret = storage->read_blocks(absolute_offset / block_size, io_bounce, 1);
        if (ret != ESP_OK) {
            return ret;
        }
        memcpy(output, io_bounce + block_offset, copy_size);
        output += copy_size;
        remaining -= copy_size;
        absolute_offset += copy_size;
    }
    return ESP_OK;
}

esp_err_t on9kvdb::write_storage_bytes_unsafe(on9kvdb_def::file_kind kind, uint32_t slot, uint64_t offset, const void *source,
                                              size_t size)
{
    if ((source == nullptr && size != 0) || storage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t region_offset = 0;
    uint64_t region_size = 0;
    esp_err_t ret = get_storage_region_unsafe(kind, slot, &region_offset, &region_size);
    if (ret != ESP_OK || offset > region_size || size > region_size - offset) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }
    if (size == 0) {
        return ESP_OK;
    }

    const uint32_t block_size = storage->block_size();
    if (block_size == 0 || on9kvdb_def::format_alignment % block_size != 0 || offset % block_size != 0 ||
        size % block_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t absolute_offset = 0;
    if (!on9kvdb_def::checked_add_u64(region_offset, offset, &absolute_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t block_count = size / block_size;
    if (block_count > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return storage->write_blocks(absolute_offset / block_size, source, static_cast<uint32_t>(block_count));
}

esp_err_t on9kvdb::sync_storage_unsafe()
{
    return storage == nullptr ? ESP_ERR_INVALID_STATE : storage->sync();
}

esp_err_t on9kvdb::storage_region_is_blank_unsafe(bool *blank_out) const
{
    if (blank_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *blank_out = false;
    if (io_frame == nullptr || storage == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const on9kvdb_def::storage_geometry geometry = get_build_geometry();
    const uint32_t block_size = storage->block_size();
    if (block_size == 0 || on9kvdb_def::format_alignment % block_size != 0 || geometry.provisioned_size % block_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    // A raw range has no mount/format operation to distinguish first use from
    // an unknown existing image. Accept only one uniformly erased byte value
    // over the entire configured range, so a wrong first LBA can never cause
    // init() to overwrite another owner's data based on a blank first sector.
    bool blank_value_known = false;
    uint8_t blank_value = 0;
    const uint64_t blocks_per_frame = on9kvdb_def::format_alignment / block_size;
    const uint64_t frame_count = geometry.provisioned_size / on9kvdb_def::format_alignment;
    for (uint64_t frame = 0; frame < frame_count; frame += 1U) {
        const esp_err_t ret = storage->read_blocks(frame * blocks_per_frame, io_frame, static_cast<uint32_t>(blocks_per_frame));
        if (ret != ESP_OK) {
            return ret;
        }
        for (size_t index = 0; index < on9kvdb_def::format_alignment; index += 1U) {
            if (!blank_value_known) {
                blank_value = io_frame[index];
                blank_value_known = blank_value == 0 || blank_value == UINT8_MAX;
            }
            if (!blank_value_known || io_frame[index] != blank_value) {
                return ESP_OK;
            }
        }
        if ((frame + 1U) % 128U == 0U) {
            vTaskDelay(1);
        }
    }
    *blank_out = blank_value_known;
    return ESP_OK;
}
