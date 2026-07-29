#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>

#include "on9kvdb.hpp"

size_t on9kvdb::descriptor_index(
    on9kvdb_def::file_kind kind, uint32_t slot)
{
    switch (kind) {
    case on9kvdb_def::file_kind::manifest:
        return slot == 0 ? 0 : storage_fd_count;
    case on9kvdb_def::file_kind::wal:
        return slot < on9kvdb_def::wal_file_count ?
            1U + slot : storage_fd_count;
    case on9kvdb_def::file_kind::table:
        return slot < CONFIG_ON9KVDB_SSTABLE_COUNT ?
            1U + on9kvdb_def::wal_file_count + slot :
            storage_fd_count;
    default:
        return storage_fd_count;
    }
}

esp_err_t on9kvdb::build_manifest_path()
{
    const size_t len = strlen(file_path);
    const char *separator =
        len > 0 && file_path[len - 1] == '/' ? "" : "/";
    const int result = snprintf(
        manifest_path, sizeof(manifest_path),
        "%s%smanifest.db", file_path, separator);
    if (result < 0 ||
        static_cast<size_t>(result) >= sizeof(manifest_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t on9kvdb::build_data_path(
    on9kvdb_def::file_kind kind, uint32_t slot,
    char *path_out, size_t path_out_len) const
{
    if (path_out == nullptr || path_out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *prefix = nullptr;
    if (kind == on9kvdb_def::file_kind::wal) {
        prefix = "wal";
    } else if (kind == on9kvdb_def::file_kind::table) {
        prefix = "table";
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t len = strlen(file_path);
    const char *separator =
        len > 0 && file_path[len - 1] == '/' ? "" : "/";
    const int result = snprintf(
        path_out, path_out_len, "%s%s%s_%" PRIu32 ".db",
        file_path, separator, prefix, slot);
    if (result < 0 ||
        static_cast<size_t>(result) >= path_out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

bool on9kvdb::parse_slot_file_name(
    const char *name, const char *prefix, uint32_t *slot_out)
{
    if (name == nullptr || prefix == nullptr || slot_out == nullptr) {
        return false;
    }

    const size_t prefix_len = strlen(prefix);
    if (strncmp(name, prefix, prefix_len) != 0) {
        return false;
    }

    const char *cursor = name + prefix_len;
    if (*cursor < '0' || *cursor > '9' ||
        (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')) {
        return false;
    }

    uint32_t slot = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        const uint32_t digit =
            static_cast<uint32_t>(*cursor - '0');
        if (slot > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        slot = slot * 10U + digit;
        cursor += 1;
    }

    if (strcmp(cursor, ".db") != 0) {
        return false;
    }

    *slot_out = slot;
    return true;
}

esp_err_t on9kvdb::verify_canonical_file_set(bool creating) const
{
    DIR *directory = opendir(file_path);
    if (directory == nullptr) {
        ESP_LOGE(TAG, "Files: opendir(%s) failed: errno=%d",
                 file_path, errno);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    errno = 0;
    while (true) {
        const struct dirent *entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                ESP_LOGE(TAG, "Files: readdir failed: errno=%d",
                         errno);
                ret = ESP_FAIL;
            }
            break;
        }

        if (strcmp(entry->d_name, "manifest.db") == 0) {
            if (creating) {
                ret = ESP_ERR_ON9KVDB_CORRUPT;
                break;
            }
            continue;
        }

        uint32_t slot = 0;
        if (parse_slot_file_name(
                entry->d_name, "wal_", &slot)) {
            if (creating || slot >= on9kvdb_def::wal_file_count) {
                ret = ESP_ERR_ON9KVDB_CORRUPT;
                break;
            }
            continue;
        }
        if (parse_slot_file_name(
                entry->d_name, "table_", &slot)) {
            if (creating ||
                slot >= manifest.geometry.table_count) {
                ret = ESP_ERR_ON9KVDB_CORRUPT;
                break;
            }
            continue;
        }

        const bool reserved_name =
            strncmp(entry->d_name, "wal_", 4) == 0 ||
            strncmp(entry->d_name, "table_", 6) == 0;
        if (reserved_name) {
            ret = ESP_ERR_ON9KVDB_CORRUPT;
            break;
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Files: canonical database namespace collision");
    }
    (void)closedir(directory);
    return ret;
}

esp_err_t on9kvdb::validate_fatfs_mount(
    uint64_t *free_bytes_out) const
{
    if (free_bytes_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    const esp_err_t ret = esp_vfs_fat_info(
        file_path, &total_bytes, &free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FATFS: mounted-volume query failed: 0x%x",
                 ret);
        return ret;
    }

    const on9kvdb_def::storage_geometry geometry =
        get_build_geometry();
    if (total_bytes < geometry.provisioned_size) {
        ESP_LOGE(TAG, "FATFS: volume is smaller than database geometry");
        return ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE;
    }

    *free_bytes_out = free_bytes;
    return ESP_OK;
}

esp_err_t on9kvdb::validate_contiguous_file(
    const char *path, uint64_t size) const
{
    if (path == nullptr || size == 0 ||
        size > on9kvdb_def::max_fat32_file_size) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat file_stat = {};
    if (stat(path, &file_stat) != 0) {
        ESP_LOGE(TAG, "File: stat(%s) failed: errno=%d",
                 path, errno);
        return ESP_ERR_ON9KVDB_CORRUPT;
    }
    if (!S_ISREG(file_stat.st_mode) || file_stat.st_size < 0 ||
        static_cast<uint64_t>(file_stat.st_size) != size) {
        ESP_LOGE(TAG, "File: invalid type or size for %s", path);
        return ESP_ERR_ON9KVDB_CORRUPT;
    }

    bool contiguous = false;
    const esp_err_t ret = esp_vfs_fat_test_contiguous_file(
        file_path, path, &contiguous);
    if (ret != ESP_OK || !contiguous) {
        ESP_LOGE(TAG, "File: non-contiguous %s: ret=0x%x",
                 path, ret);
        return ret == ESP_OK ?
            ESP_ERR_ON9KVDB_CORRUPT : ret;
    }

    return ESP_OK;
}

esp_err_t on9kvdb::provision_contiguous_file(
    const char *path, uint64_t size, bool *created_out) const
{
    if (path == nullptr || created_out == nullptr || size == 0 ||
        size > on9kvdb_def::max_fat32_file_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *created_out = false;
    struct stat file_stat = {};
    const int stat_result = stat(path, &file_stat);
    if (stat_result == 0 && file_stat.st_size > 0) {
        return validate_contiguous_file(path, size);
    }
    if (stat_result == 0 && !S_ISREG(file_stat.st_mode)) {
        return ESP_ERR_ON9KVDB_CORRUPT;
    }
    if (stat_result != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "File: stat(%s) failed: errno=%d",
                 path, errno);
        return ESP_FAIL;
    }

    uint64_t free_bytes = 0;
    esp_err_t ret = validate_fatfs_mount(&free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    if (free_bytes < size) {
        return ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE;
    }

    ret = esp_vfs_fat_create_contiguous_file(
        file_path, path, size, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "File: contiguous create failed for %s: "
                      "ret=0x%x errno=%d",
                 path, ret, errno);
        return errno == ENOSPC ?
            ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE : ret;
    }

    *created_out = true;
    return validate_contiguous_file(path, size);
}

esp_err_t on9kvdb::open_file(
    const char *path, int *fd_out) const
{
    if (path == nullptr || fd_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const int opened_fd = ::open(path, O_RDWR);
    if (opened_fd < 0) {
        ESP_LOGE(TAG, "File: open(%s) failed: errno=%d; "
                      "check FATFS max_files",
                 path, errno);
        return ESP_FAIL;
    }

    *fd_out = opened_fd;
    return ESP_OK;
}

esp_err_t on9kvdb::read_exact_fd(
    int file_fd, uint64_t file_size, uint64_t offset,
    void *buf_out, size_t len) const
{
    if (len == 0) {
        return ESP_OK;
    }
    if (file_fd < 0 || buf_out == nullptr || len > file_size ||
        offset > file_size - len ||
        offset > static_cast<uint64_t>(
            std::numeric_limits<off_t>::max())) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *out = static_cast<uint8_t *>(buf_out);
    size_t remaining = len;
    uint64_t position = offset;
    while (remaining > 0) {
        const ssize_t result = pread(
            file_fd, out, remaining,
            static_cast<off_t>(position));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            ESP_LOGE(TAG, "File: exact read failed: errno=%d",
                     errno);
            return ESP_FAIL;
        }

        out += result;
        remaining -= static_cast<size_t>(result);
        position += static_cast<size_t>(result);
    }

    return ESP_OK;
}

esp_err_t on9kvdb::write_exact_fd(
    int file_fd, uint64_t file_size, uint64_t offset,
    const void *buf, size_t len) const
{
    if (len == 0) {
        return ESP_OK;
    }
    if (file_fd < 0 || buf == nullptr || len > file_size ||
        offset > file_size - len ||
        offset > static_cast<uint64_t>(
            std::numeric_limits<off_t>::max())) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *in = static_cast<const uint8_t *>(buf);
    size_t remaining = len;
    uint64_t position = offset;
    while (remaining > 0) {
        const ssize_t result = pwrite(
            file_fd, in, remaining,
            static_cast<off_t>(position));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            ESP_LOGE(TAG, "File: exact write failed: errno=%d",
                     errno);
            return ESP_FAIL;
        }

        in += result;
        remaining -= static_cast<size_t>(result);
        position += static_cast<size_t>(result);
    }

    return ESP_OK;
}

esp_err_t on9kvdb::sync_fd(int file_fd) const
{
    if (file_fd < 0 || fsync(file_fd) != 0) {
        ESP_LOGE(TAG, "File: fsync failed: errno=%d", errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}
