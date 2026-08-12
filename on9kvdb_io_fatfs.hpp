#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_vfs.h>

#include "on9kvdb_io.hpp"

/**
 * @brief One-contiguous-file FATFS implementation of the on9kvdb block interface.
 *
 * init() provisions or opens exactly one file below an application-mounted
 * FATFS base path. When the file is absent, it is created with
 * esp_vfs_fat_create_contiguous_file(..., true); when it already exists, its
 * size and contiguity are verified instead. The adapter never mounts,
 * formats, enumerates, deletes, resizes, or accesses any other file.
 *
 * The adapter deliberately exposes 4096-byte logical blocks, regardless of
 * FATFS's underlying sector size. This matches every on9kvdb persistent write
 * boundary and avoids read-modify-write activity in the database layer.
 */
class on9kvdb_io_fatfs final : public on9kvdb_io
{
public:
    /** @brief Largest accepted filename, excluding its terminating null byte. */
    static const constexpr size_t maximum_file_name_length = 64;

    on9kvdb_io_fatfs() = default;
    on9kvdb_io_fatfs(const on9kvdb_io_fatfs &) = delete;
    on9kvdb_io_fatfs &operator=(const on9kvdb_io_fatfs &) = delete;

    /**
     * @brief Provision or open one contiguous database file.
     *
     * @param base_path Already mounted FATFS VFS path, such as @c "/kvdb".
     * @param file_name Simple filename below @p base_path; path separators,
     * @c ".", and @c ".." are rejected so this object has exactly one target.
     * @param file_size Exact database-file capacity in bytes. It must be a
     * non-zero multiple of 4096 and fit the VFS @c off_t range.
     * @return ESP_OK on success, ESP_ERR_INVALID_STATE when already initialized,
     * ESP_ERR_INVALID_ARG/ESP_ERR_INVALID_SIZE for invalid input, or a FATFS/
     * VFS error when creation, validation, or opening fails.
     *
     * @note A non-empty existing file is never resized or replaced. It must
     * already have exactly @p file_size bytes and be contiguous.
     */
    esp_err_t init(const char *base_path, const char *file_name, uint64_t file_size);

    /**
     * @brief Close the one database file without deleting or modifying it.
     *
     * Call this after on9kvdb::deinit(). A caller that needs a durability
     * barrier before closing must call sync() and check its result first.
     */
    esp_err_t deinit();

    /** @copydoc on9kvdb_io::block_size */
    uint32_t block_size() const override;

    /** @copydoc on9kvdb_io::block_count */
    uint64_t block_count() const override;

    /** @copydoc on9kvdb_io::read_blocks */
    esp_err_t read_blocks(uint64_t start_block, void *destination, uint32_t block_count) override;

    /** @copydoc on9kvdb_io::write_blocks */
    esp_err_t write_blocks(uint64_t start_block, const void *source, uint32_t block_count) override;

    /** @copydoc on9kvdb_io::sync */
    esp_err_t sync() override;

private:
    static const constexpr uint32_t logical_block_size = 4096;
    static const constexpr size_t maximum_base_path_length = ESP_VFS_PATH_MAX;
    static const constexpr size_t maximum_full_path_length = maximum_base_path_length + 1U + maximum_file_name_length + 1U;

    esp_err_t build_paths(const char *base_path, const char *file_name);
    bool range_is_valid(uint64_t start_block, uint32_t block_count) const;
    bool get_byte_range(uint64_t start_block, uint32_t block_count, int64_t *offset_out, size_t *size_out) const;
    esp_err_t validate_existing_file(uint64_t file_size) const;
    esp_err_t read_exact(int64_t offset, void *destination, size_t size) const;
    esp_err_t write_exact(int64_t offset, const void *source, size_t size) const;
    void clear_state();

private:
    int file_descriptor = -1;
    uint64_t visible_block_count = 0;
    char mounted_base_path[maximum_base_path_length + 1U] = {};
    char file_path[maximum_full_path_length] = {};
};
