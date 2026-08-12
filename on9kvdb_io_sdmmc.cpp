#include <limits.h>

#include "on9kvdb_io_sdmmc.hpp"

esp_err_t on9kvdb_io_sdmmc::init(sdmmc_card_t *new_card, uint32_t new_first_block)
{
    if (new_card == nullptr || new_card->csd.capacity <= 0 || new_card->csd.sector_size <= 0 ||
        static_cast<uint64_t>(new_first_block) >= static_cast<uint64_t>(new_card->csd.capacity)) {
        return ESP_ERR_INVALID_ARG;
    }

    card = new_card;
    first_block = new_first_block;
    visible_block_count = static_cast<uint64_t>(new_card->csd.capacity) - first_block;
    native_block_size = static_cast<uint32_t>(new_card->csd.sector_size);
    return ESP_OK;
}

uint32_t on9kvdb_io_sdmmc::block_size() const
{
    return native_block_size;
}

uint64_t on9kvdb_io_sdmmc::block_count() const
{
    return visible_block_count;
}

bool on9kvdb_io_sdmmc::range_is_valid(uint64_t start_block, uint32_t count) const
{
    // start_block is relative to the exclusive database suffix. Check both
    // that suffix and the size_t LBA used by ESP-IDF before adding the offset.
    return card != nullptr && native_block_size != 0 && start_block <= visible_block_count &&
           count <= visible_block_count - start_block && first_block <= UINT64_MAX - start_block &&
           first_block + start_block <= static_cast<uint64_t>(SIZE_MAX);
}

esp_err_t on9kvdb_io_sdmmc::read_blocks(uint64_t start_block, void *destination, uint32_t count)
{
    if ((destination == nullptr && count != 0) || !range_is_valid(start_block, count)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return ESP_OK;
    }
    return sdmmc_read_sectors(card, destination, static_cast<size_t>(first_block + start_block), count);
}

esp_err_t on9kvdb_io_sdmmc::write_blocks(uint64_t start_block, const void *source, uint32_t count)
{
    if ((source == nullptr && count != 0) || !range_is_valid(start_block, count)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 0) {
        return ESP_OK;
    }
    return sdmmc_write_sectors(card, source, static_cast<size_t>(first_block + start_block), count);
}

esp_err_t on9kvdb_io_sdmmc::sync()
{
    // SDMMC read/write APIs return after the card command completes. A status command is the only public API-level barrier
    // available to this backend; it detects card/transport errors but is not a claim about capacitor-backed power-loss safety.
    return card == nullptr ? ESP_ERR_INVALID_STATE : sdmmc_get_status(card);
}
