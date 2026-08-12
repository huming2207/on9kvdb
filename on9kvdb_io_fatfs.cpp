#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_vfs_fat.h>

#include "on9kvdb_io_fatfs.hpp"

// All filesystem metadata work is intentionally confined to init(); normal I/O uses one open descriptor.
esp_err_t on9kvdb_io_fatfs::build_paths(const char *base_path, const char *file_name)
{
    if (base_path == nullptr || file_name == nullptr || base_path[0] != '/' || file_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t base_length = strnlen(base_path, maximum_base_path_length + 1U);
    const size_t name_length = strnlen(file_name, maximum_file_name_length + 1U);
    if (base_length == 0 || base_length > maximum_base_path_length || name_length == 0 ||
        name_length > maximum_file_name_length || (name_length == 1U && file_name[0] == '.') ||
        (name_length == 2U && file_name[0] == '.' && file_name[1] == '.')) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index = 0; index < name_length; index += 1U) {
        // The target must be one child of the mounted base, never a path.
        if (file_name[index] == '/' || file_name[index] == '\\') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const bool has_separator = base_path[base_length - 1U] == '/';
    const size_t full_length = base_length + (has_separator ? 0U : 1U) + name_length;
    if (full_length >= sizeof(file_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(mounted_base_path, base_path, base_length);
    mounted_base_path[base_length] = '\0';
    memcpy(file_path, base_path, base_length);
    size_t path_offset = base_length;
    if (!has_separator) {
        file_path[path_offset++] = '/';
    }
    memcpy(file_path + path_offset, file_name, name_length);
    file_path[full_length] = '\0';
    return ESP_OK;
}

void on9kvdb_io_fatfs::clear_state()
{
    file_descriptor = -1;
    visible_block_count = 0;
    mounted_base_path[0] = '\0';
    file_path[0] = '\0';
}

esp_err_t on9kvdb_io_fatfs::validate_existing_file(uint64_t file_size) const
{
    struct stat file_stat = {};
    if (stat(file_path, &file_stat) != 0) {
        return ESP_FAIL;
    }
    if (!S_ISREG(file_stat.st_mode) || file_stat.st_size < 0 || static_cast<uint64_t>(file_stat.st_size) != file_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    // A matching byte length alone does not stop FATFS from scattering I/O.
    bool contiguous = false;
    const esp_err_t ret = esp_vfs_fat_test_contiguous_file(mounted_base_path, file_path, &contiguous);
    return ret == ESP_OK && contiguous ? ESP_OK : (ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret);
}

esp_err_t on9kvdb_io_fatfs::init(const char *base_path, const char *file_name, uint64_t file_size)
{
    if (file_descriptor >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (file_size == 0 || file_size % logical_block_size != 0 ||
        file_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return ESP_ERR_INVALID_SIZE;
    }

    clear_state();
    esp_err_t ret = build_paths(base_path, file_name);
    if (ret != ESP_OK) {
        clear_state();
        return ret;
    }

    struct stat file_stat = {};
    if (stat(file_path, &file_stat) != 0) {
        if (errno != ENOENT) {
            clear_state();
            return ESP_FAIL;
        }
        // This is the only call which may create or allocate a filesystem file.
        ret = esp_vfs_fat_create_contiguous_file(mounted_base_path, file_path, file_size, true);
        if (ret != ESP_OK) {
            clear_state();
            return ret;
        }
    }

    ret = validate_existing_file(file_size);
    if (ret != ESP_OK) {
        clear_state();
        return ret;
    }

    const int descriptor = open(file_path, O_RDWR);
    if (descriptor < 0) {
        clear_state();
        return ESP_FAIL;
    }

    file_descriptor = descriptor;
    visible_block_count = file_size / logical_block_size;
    return ESP_OK;
}

esp_err_t on9kvdb_io_fatfs::deinit()
{
    if (file_descriptor < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const int descriptor = file_descriptor;
    clear_state();
    return close(descriptor) == 0 ? ESP_OK : ESP_FAIL;
}

uint32_t on9kvdb_io_fatfs::block_size() const
{
    return logical_block_size;
}

uint64_t on9kvdb_io_fatfs::block_count() const
{
    return visible_block_count;
}

bool on9kvdb_io_fatfs::range_is_valid(uint64_t start_block, uint32_t count) const
{
    return file_descriptor >= 0 && start_block <= visible_block_count && count <= visible_block_count - start_block;
}

bool on9kvdb_io_fatfs::get_byte_range(uint64_t start_block, uint32_t count, int64_t *offset_out, size_t *size_out) const
{
    if (offset_out == nullptr || size_out == nullptr || !range_is_valid(start_block, count) ||
        start_block > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) / logical_block_size ||
        count > std::numeric_limits<size_t>::max() / logical_block_size) {
        return false;
    }
    *offset_out = static_cast<int64_t>(start_block * logical_block_size);
    *size_out = static_cast<size_t>(count) * logical_block_size;
    return true;
}

esp_err_t on9kvdb_io_fatfs::read_exact(int64_t offset, void *destination, size_t size) const
{
    uint8_t *output = static_cast<uint8_t *>(destination);
    size_t remaining = size;
    int64_t position = offset;
    while (remaining > 0) {
        const size_t request = remaining < static_cast<size_t>(std::numeric_limits<ssize_t>::max())
                                   ? remaining
                                   : static_cast<size_t>(std::numeric_limits<ssize_t>::max());
        const ssize_t result = pread(file_descriptor, output, request, static_cast<off_t>(position));
        // POSIX I/O may be interrupted or complete only part of a request.
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        output += result;
        remaining -= static_cast<size_t>(result);
        position += result;
    }
    return ESP_OK;
}

esp_err_t on9kvdb_io_fatfs::write_exact(int64_t offset, const void *source, size_t size) const
{
    const uint8_t *input = static_cast<const uint8_t *>(source);
    size_t remaining = size;
    int64_t position = offset;
    while (remaining > 0) {
        const size_t request = remaining < static_cast<size_t>(std::numeric_limits<ssize_t>::max())
                                   ? remaining
                                   : static_cast<size_t>(std::numeric_limits<ssize_t>::max());
        const ssize_t result = pwrite(file_descriptor, input, request, static_cast<off_t>(position));
        // Continue until all blocks reach the VFS file descriptor.
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        input += result;
        remaining -= static_cast<size_t>(result);
        position += result;
    }
    return ESP_OK;
}

esp_err_t on9kvdb_io_fatfs::read_blocks(uint64_t start_block, void *destination, uint32_t count)
{
    if (destination == nullptr && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return range_is_valid(start_block, count) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    int64_t offset = 0;
    size_t size = 0;
    return get_byte_range(start_block, count, &offset, &size) ? read_exact(offset, destination, size) : ESP_ERR_INVALID_ARG;
}

esp_err_t on9kvdb_io_fatfs::write_blocks(uint64_t start_block, const void *source, uint32_t count)
{
    if (source == nullptr && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return range_is_valid(start_block, count) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    int64_t offset = 0;
    size_t size = 0;
    return get_byte_range(start_block, count, &offset, &size) ? write_exact(offset, source, size) : ESP_ERR_INVALID_ARG;
}

esp_err_t on9kvdb_io_fatfs::sync()
{
    if (file_descriptor < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    return fsync(file_descriptor) == 0 ? ESP_OK : ESP_FAIL;
}
