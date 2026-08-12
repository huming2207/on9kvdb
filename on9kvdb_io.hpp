#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

/**
 * @brief Fixed-size raw block device used by an on9kvdb instance.
 *
 * The device exposes only the LBA range owned exclusively by one database:
 * block zero is the first byte of that range, not necessarily the first LBA
 * of the physical medium. Implementations are non-owning. The caller must
 * initialize the underlying transport before on9kvdb::init() and keep it
 * alive until on9kvdb::deinit() returns.
 */
class on9kvdb_io
{
public:
    virtual ~on9kvdb_io() = default;

    /** @brief Return the fixed native block size in bytes. */
    virtual uint32_t block_size() const = 0;

    /** @brief Return the number of database-visible native blocks. */
    virtual uint64_t block_count() const = 0;

    /**
     * @brief Read complete, consecutive native blocks.
     *
     * @param start_block First database-visible block to read.
     * @param destination Buffer holding at least @p block_count times
     * block_size() bytes.
     * @param block_count Number of blocks to read.
     * @return ESP_OK on success or an ESP-IDF transport/range error.
     */
    virtual esp_err_t read_blocks(uint64_t start_block, void *destination, uint32_t block_count) = 0;

    /**
     * @brief Write complete, consecutive native blocks.
     *
     * @param start_block First database-visible block to write.
     * @param source Buffer holding @p block_count times block_size() bytes.
     * @param block_count Number of blocks to write.
     * @return ESP_OK on success or an ESP-IDF transport/range error.
     */
    virtual esp_err_t write_blocks(uint64_t start_block, const void *source, uint32_t block_count) = 0;

    /**
     * @brief Complete the device's configured write-status barrier.
     *
     * on9kvdb calls this before publishing WAL or manifest reachability. A
     * backend must report transport failure, but device-specific physical
     * power-loss guarantees remain a qualification responsibility.
     */
    virtual esp_err_t sync() = 0;
};
