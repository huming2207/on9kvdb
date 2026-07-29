#include <cerrno>
#include <cstring>
#include <inttypes.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_random.h>

#include "on9kvdb.hpp"

namespace
{
    bool identity_matches(const on9kvdb_def::file_identity &identity, on9kvdb_def::file_kind kind, uint64_t database_id,
                          uint64_t file_size, uint32_t slot)
    {
        return identity.kind == kind && identity.generation == 1 && identity.database_id == database_id &&
               identity.file_size == file_size && identity.slot == slot;
    }
}

esp_err_t on9kvdb::setup_manifest()
{
    struct stat file_stat = {};
    const int stat_result = stat(manifest_path, &file_stat);
    if (stat_result == 0) {
        if (!S_ISREG(file_stat.st_mode) || file_stat.st_size <= 0) {
            return ESP_ERR_INVALID_CRC;
        }
        return open_existing_manifest();
    }
    if (errno != ENOENT) {
        ESP_LOGE(TAG, "Manifest: stat failed: errno=%d", errno);
        return ESP_FAIL;
    }

    return create_manifest();
}

esp_err_t on9kvdb::create_manifest()
{
    esp_err_t ret = verify_canonical_file_set(true);
    if (ret != ESP_OK) {
        return ret;
    }

    uint64_t free_bytes = 0;
    ret = validate_fatfs_mount(&free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    const on9kvdb_def::storage_geometry geometry = get_build_geometry();
    if (free_bytes < geometry.provisioned_size) {
        return ESP_ERR_NO_MEM;
    }

    bool created = false;
    ret = provision_contiguous_file(manifest_path, on9kvdb_def::manifest_file_size, &created);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!created) {
        return ESP_ERR_INVALID_CRC;
    }

    const size_t fd_index = descriptor_index(on9kvdb_def::file_kind::manifest, 0);
    ret = open_file(manifest_path, &storage_fds[fd_index]);
    if (ret != ESP_OK) {
        return ret;
    }

    uint64_t database_id = 0;
    while (database_id == 0) {
        database_id = (static_cast<uint64_t>(esp_random()) << 32U) | esp_random();
    }

    manifest = {};
    manifest.database_id = database_id;
    manifest.geometry = geometry;
    manifest.limits = get_build_limits();
    manifest.active_wal_slot = 0;
    manifest_valid_copy_count = 0;
    ret = write_manifest_copy(1, on9kvdb_def::manifest_state_provisioning_owned);
    if (ret != ESP_OK) {
        return ret;
    }
    manifest_valid_copy_count = 1;

    ret = write_manifest_copy(2, on9kvdb_def::manifest_state_provisioning_owned);
    if (ret == ESP_OK) {
        manifest_valid_copy_count = 2;
    }
    return ret;
}

esp_err_t on9kvdb::open_existing_manifest()
{
    esp_err_t ret = validate_contiguous_file(manifest_path, on9kvdb_def::manifest_file_size);
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t fd_index = descriptor_index(on9kvdb_def::file_kind::manifest, 0);
    ret = open_file(manifest_path, &storage_fds[fd_index]);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = load_manifest();
    if (ret != ESP_OK) {
        return ret;
    }

    const on9kvdb_def::storage_geometry build_geometry = get_build_geometry();
    if (!on9kvdb_def::storage_geometry_equal(manifest.geometry, build_geometry) ||
        !on9kvdb_def::logical_limits_equal(manifest.limits, get_build_limits())) {
        ESP_LOGE(TAG, "Manifest: Kconfig geometry/limit mismatch; "
                      "delete and recreate the complete database");
        return ESP_ERR_INVALID_SIZE;
    }

    ret = verify_canonical_file_set(false);
    if (ret != ESP_OK) {
        return ret;
    }

    if (manifest.state == on9kvdb_def::manifest_state_ready && manifest_valid_copy_count < on9kvdb_def::manifest_slot_count) {
        ESP_LOGW(TAG, "Manifest: one redundant copy is invalid");
    }

    if (manifest.state == on9kvdb_def::manifest_state_provisioning_owned &&
        manifest_valid_copy_count < on9kvdb_def::manifest_slot_count) {
        if (manifest.generation == UINT64_MAX) {
            return ESP_ERR_INVALID_CRC;
        }
        ret = write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_provisioning_owned);
        if (ret == ESP_OK) {
            manifest_valid_copy_count = on9kvdb_def::manifest_slot_count;
        }
    }

    return ret;
}

esp_err_t on9kvdb::load_manifest()
{
    if (io_frame == nullptr || future_scratch == nullptr || future_scratch_size < 2U * sizeof(on9kvdb_def::manifest_record)) {
        return ESP_ERR_INVALID_STATE;
    }

    const int manifest_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::manifest, 0)];
    bool found = false;
    bool newer_version_found = false;
    auto *valid = reinterpret_cast<on9kvdb_def::manifest_record *>(future_scratch);
    on9kvdb_def::manifest_record *selected = nullptr;
    uint32_t selected_slot = 0;
    bool stabilization_required = false;
    bool slot_valid[on9kvdb_def::manifest_slot_count] = {};
    for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
        new (&valid[slot]) on9kvdb_def::manifest_record{};
    }

    for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
        const uint64_t offset = static_cast<uint64_t>(slot) * on9kvdb_def::manifest_slot_size;
        esp_err_t ret =
            read_exact_fd(manifest_fd, on9kvdb_def::manifest_file_size, offset, io_frame, on9kvdb_def::manifest_record_size);
        if (ret != ESP_OK) {
            return ret;
        }

        const on9kvdb_def::format_status status =
            on9kvdb_def::decode_manifest_record(io_frame, on9kvdb_def::manifest_record_size, &valid[slot]);
        if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
            newer_version_found = true;
            continue;
        }
        if (status != on9kvdb_def::format_status::ok) {
            continue;
        }
        const uint32_t expected_slot = static_cast<uint32_t>((valid[slot].generation - 1U) % on9kvdb_def::manifest_slot_count);
        if (expected_slot != slot) {
            return ESP_ERR_INVALID_CRC;
        }

        slot_valid[slot] = true;
        if (!found || valid[slot].generation > selected->generation) {
            selected = &valid[slot];
            selected_slot = slot;
            found = true;
        }
    }

    if (newer_version_found) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (!found) {
        return ESP_ERR_INVALID_CRC;
    }

    // Both copies describe the same database. Only an adjacent generation is valid because each manifest update replaces one
    // alternating slot after the previous generation has already reached durable storage.
    for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
        if (!slot_valid[slot]) {
            continue;
        }
        if (valid[slot].database_id != selected->database_id ||
            !on9kvdb_def::storage_geometry_equal(valid[slot].geometry, selected->geometry) ||
            !on9kvdb_def::logical_limits_equal(valid[slot].limits, selected->limits) ||
            (valid[slot].generation != selected->generation && selected->generation - valid[slot].generation != 1U) ||
            valid[slot].next_table_generation > selected->next_table_generation ||
            valid[slot].safe_checkpoint_sequence > selected->safe_checkpoint_sequence ||
            (valid[slot].generation == selected->generation && valid[slot].state != selected->state) ||
            (valid[slot].generation < selected->generation && valid[slot].state == on9kvdb_def::manifest_state_ready &&
             selected->state == on9kvdb_def::manifest_state_provisioning_owned)) {
            return ESP_ERR_INVALID_CRC;
        }
        if (valid[slot].generation < selected->generation) {
            if (valid[slot].active_table_bank == selected->active_table_bank) {
                for (uint32_t table_slot = 0; table_slot < selected->geometry.table_count; table_slot += 1) {
                    if (valid[slot].tables[table_slot].active &&
                        !on9kvdb_def::table_reference_equal(valid[slot].tables[table_slot], selected->tables[table_slot])) {
                        return ESP_ERR_INVALID_CRC;
                    }
                }
            }
            stabilization_required = valid[slot].active_table_bank != selected->active_table_bank ||
                                     valid[slot].active_wal_slot != selected->active_wal_slot ||
                                     valid[slot].wal_generation[0] != selected->wal_generation[0] ||
                                     valid[slot].wal_generation[1] != selected->wal_generation[1];
        }
    }

    manifest = *selected;
    manifest_slot = selected_slot;
    manifest_valid_copy_count = 0;
    for (uint32_t slot = 0; slot < on9kvdb_def::manifest_slot_count; slot += 1) {
        if (slot_valid[slot]) {
            manifest_valid_copy_count += 1;
        }
    }
    manifest_stabilization_required = stabilization_required || manifest_valid_copy_count < on9kvdb_def::manifest_slot_count;
    return ESP_OK;
}

esp_err_t on9kvdb::write_manifest_copy(uint64_t generation, uint16_t state)
{
    if (generation == 0 || manifest.database_id == 0 || manifest.generation == UINT64_MAX ||
        generation != manifest.generation + 1U || future_scratch == nullptr ||
        future_scratch_size < sizeof(on9kvdb_def::manifest_record)) {
        return ESP_ERR_INVALID_STATE;
    }

    auto *next = new (future_scratch) on9kvdb_def::manifest_record(manifest);
    next->generation = generation;
    next->state = state;

    if (io_frame == nullptr || !on9kvdb_def::encode_manifest_record(io_frame, on9kvdb_def::manifest_record_size, *next)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t slot = static_cast<uint32_t>((generation - 1U) % on9kvdb_def::manifest_slot_count);
    const uint64_t offset = static_cast<uint64_t>(slot) * on9kvdb_def::manifest_slot_size;
    const int manifest_fd = storage_fds[descriptor_index(on9kvdb_def::file_kind::manifest, 0)];
    esp_err_t ret =
        write_exact_fd(manifest_fd, on9kvdb_def::manifest_file_size, offset, io_frame, on9kvdb_def::manifest_record_size);
    if (ret == ESP_OK) {
        ret = sync_fd(manifest_fd);
    }
    if (ret == ESP_OK) {
        manifest = *next;
        manifest_slot = slot;
    } else {
        // A failed write or sync cannot prove whether this generation reached durable storage. Stop normal operations so no
        // file selected by a possibly valid copy can be reused before deinit/re-init performs recovery.
        storage_faulted = true;
    }

    return ret;
}

esp_err_t on9kvdb::stabilize_manifest_unsafe()
{
    if (!manifest_stabilization_required) {
        return ESP_OK;
    }
    if (manifest.generation == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = write_manifest_copy(manifest.generation + 1U, manifest.state);
    if (ret == ESP_OK) {
        manifest_valid_copy_count = on9kvdb_def::manifest_slot_count;
        manifest_stabilization_required = false;
    }
    return ret;
}

esp_err_t on9kvdb::write_file_identity_copy(int file_fd, on9kvdb_def::file_kind kind, uint32_t slot, uint32_t copy_slot) const
{
    if (copy_slot >= on9kvdb_def::identity_slot_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (kind != on9kvdb_def::file_kind::wal && kind != on9kvdb_def::file_kind::table) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t file_size = kind == on9kvdb_def::file_kind::wal ? manifest.geometry.wal_size : manifest.geometry.table_size;
    on9kvdb_def::file_identity identity = {};
    identity.generation = 1;
    identity.database_id = manifest.database_id;
    identity.file_size = file_size;
    identity.slot = slot;
    identity.kind = kind;

    uint8_t encoded[on9kvdb_def::file_identity_size] = {};
    if (!on9kvdb_def::encode_file_identity(encoded, sizeof(encoded), identity)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint64_t offset = static_cast<uint64_t>(copy_slot) * on9kvdb_def::identity_slot_size;
    esp_err_t ret = write_exact_fd(file_fd, file_size, offset, encoded, sizeof(encoded));
    if (ret == ESP_OK) {
        ret = sync_fd(file_fd);
    }
    return ret;
}

esp_err_t on9kvdb::load_file_identity(int file_fd, on9kvdb_def::file_kind kind, uint32_t slot,
                                      bool valid_copies[on9kvdb_def::identity_slot_count]) const
{
    if (valid_copies == nullptr || (kind != on9kvdb_def::file_kind::wal && kind != on9kvdb_def::file_kind::table)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t file_size = kind == on9kvdb_def::file_kind::wal ? manifest.geometry.wal_size : manifest.geometry.table_size;
    bool any_valid = false;
    for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::identity_slot_count; copy_slot += 1) {
        valid_copies[copy_slot] = false;
        uint8_t encoded[on9kvdb_def::file_identity_size] = {};
        const uint64_t offset = static_cast<uint64_t>(copy_slot) * on9kvdb_def::identity_slot_size;
        esp_err_t ret = read_exact_fd(file_fd, file_size, offset, encoded, sizeof(encoded));
        if (ret != ESP_OK) {
            return ret;
        }

        on9kvdb_def::file_identity identity = {};
        const on9kvdb_def::format_status status = on9kvdb_def::decode_file_identity(encoded, sizeof(encoded), kind, &identity);
        if (status == on9kvdb_def::format_status::new_version || status == on9kvdb_def::format_status::invalid_revision) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (status != on9kvdb_def::format_status::ok) {
            continue;
        }
        if (!identity_matches(identity, kind, manifest.database_id, file_size, slot)) {
            return ESP_ERR_INVALID_CRC;
        }

        valid_copies[copy_slot] = true;
        any_valid = true;
    }

    return any_valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t on9kvdb::provision_one_data_file(on9kvdb_def::file_kind kind, uint32_t slot)
{
    if (kind != on9kvdb_def::file_kind::wal && kind != on9kvdb_def::file_kind::table) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t file_size = kind == on9kvdb_def::file_kind::wal ? manifest.geometry.wal_size : manifest.geometry.table_size;
    char path[PATH_MAX] = {};
    esp_err_t ret = build_data_path(kind, slot, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    bool created = false;
    if (manifest.state == on9kvdb_def::manifest_state_provisioning_owned) {
        ret = provision_contiguous_file(path, file_size, &created);
    } else if (manifest.state == on9kvdb_def::manifest_state_ready) {
        ret = validate_contiguous_file(path, file_size);
    } else {
        return ESP_ERR_INVALID_CRC;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t fd_index = descriptor_index(kind, slot);
    if (fd_index >= storage_fd_count || storage_fds[fd_index] >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    ret = open_file(path, &storage_fds[fd_index]);
    if (ret != ESP_OK) {
        return ret;
    }

    bool valid_copies[on9kvdb_def::identity_slot_count] = {};
    ret = load_file_identity(storage_fds[fd_index], kind, slot, valid_copies);
    if (ret == ESP_ERR_NOT_FOUND && manifest.state == on9kvdb_def::manifest_state_provisioning_owned) {
        ret = ESP_OK;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = ESP_ERR_INVALID_CRC;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (manifest.state == on9kvdb_def::manifest_state_provisioning_owned && kind == on9kvdb_def::file_kind::table) {
        // FatFs f_expand() allocates clusters but does not initialize their sectors. Explicit zero publication markers let
        // Phase 4 distinguish a never-published slot from a slot that must not be overwritten after manifest-copy fallback.
        memset(io_frame, 0, on9kvdb_def::wal_frame_size);
        for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::table_header_slot_count; copy_slot += 1) {
            const uint64_t offset =
                on9kvdb_def::table_header_region_offset + static_cast<uint64_t>(copy_slot) * on9kvdb_def::table_header_slot_size;
            ret = write_exact_fd(storage_fds[fd_index], file_size, offset, io_frame, on9kvdb_def::table_header_slot_size);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        ret = write_exact_fd(storage_fds[fd_index], file_size, file_size - on9kvdb_def::table_footer_slot_size, io_frame,
                             on9kvdb_def::table_footer_slot_size);
        if (ret == ESP_OK) {
            ret = sync_fd(storage_fds[fd_index]);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (manifest.state == on9kvdb_def::manifest_state_provisioning_owned) {
        for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::identity_slot_count; copy_slot += 1) {
            if (valid_copies[copy_slot]) {
                continue;
            }
            ret = write_file_identity_copy(storage_fds[fd_index], kind, slot, copy_slot);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        bool final_copies[on9kvdb_def::identity_slot_count] = {};
        ret = load_file_identity(storage_fds[fd_index], kind, slot, final_copies);
        if (ret != ESP_OK) {
            return ret;
        }
        for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::identity_slot_count; copy_slot += 1) {
            if (!final_copies[copy_slot]) {
                return ESP_ERR_INVALID_CRC;
            }
        }
    } else {
        uint32_t valid_count = 0;
        for (uint32_t copy_slot = 0; copy_slot < on9kvdb_def::identity_slot_count; copy_slot += 1) {
            if (valid_copies[copy_slot]) {
                valid_count += 1;
            }
        }
        if (valid_count < on9kvdb_def::identity_slot_count) {
            ESP_LOGW(TAG,
                     "File: one identity copy is invalid: "
                     "kind=%u slot=%" PRIu32,
                     static_cast<unsigned>(kind), slot);
        }
    }

    (void)created;
    return ESP_OK;
}

esp_err_t on9kvdb::provision_all_data_files()
{
    for (uint32_t slot = 0; slot < manifest.geometry.wal_count; slot += 1) {
        esp_err_t ret = provision_one_data_file(on9kvdb_def::file_kind::wal, slot);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    for (uint32_t slot = 0; slot < manifest.geometry.table_count; slot += 1) {
        esp_err_t ret = provision_one_data_file(on9kvdb_def::file_kind::table, slot);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (manifest.state == on9kvdb_def::manifest_state_ready) {
        return ESP_OK;
    }
    if (manifest.state != on9kvdb_def::manifest_state_provisioning_owned || manifest.generation == UINT64_MAX) {
        return ESP_ERR_INVALID_CRC;
    }

    esp_err_t ret = initialise_first_wal();
    if (ret != ESP_OK) {
        return ret;
    }

    manifest.active_wal_slot = 0;
    manifest.wal_generation[0] = 1;
    manifest.wal_generation[1] = 0;
    manifest.safe_checkpoint_sequence = 0;
    return write_manifest_copy(manifest.generation + 1U, on9kvdb_def::manifest_state_ready);
}
