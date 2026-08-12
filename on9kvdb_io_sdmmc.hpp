#pragma once

#include <cstdint>

#include <sdmmc_cmd.h>

#include "on9kvdb_io.hpp"

/**
 * @brief Native-SDMMC implementation of the on9kvdb raw block-device interface.
 *
 * init() maps an exclusive suffix of an already initialized SD/MMC card into
 * database-visible block zero. It neither initializes/deinitializes the host
 * nor mounts, formats, or otherwise interprets the card as a filesystem.
 */
class on9kvdb_io_sdmmc final : public on9kvdb_io
{
public:
    on9kvdb_io_sdmmc() = default;

    /**
     * @brief Bind this device to an initialized card and an exclusive first LBA.
     *
     * @param card Initialized SD/MMC card. It must outlive this object while
     * the database is open.
     * @param first_block First physical card sector reserved for on9kvdb.
     * @return ESP_OK on success or ESP_ERR_INVALID_ARG when the card geometry
     * or requested first LBA is invalid.
     */
    esp_err_t init(sdmmc_card_t *card, uint32_t first_block);

    uint32_t block_size() const override;
    uint64_t block_count() const override;
    esp_err_t read_blocks(uint64_t start_block, void *destination, uint32_t block_count) override;
    esp_err_t write_blocks(uint64_t start_block, const void *source, uint32_t block_count) override;
    esp_err_t sync() override;

private:
    bool range_is_valid(uint64_t start_block, uint32_t block_count) const;

private:
    sdmmc_card_t *card = nullptr;
    uint64_t first_block = 0;
    uint64_t visible_block_count = 0;
    uint32_t native_block_size = 0;
};
