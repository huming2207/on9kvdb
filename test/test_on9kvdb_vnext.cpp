#include <type_traits>

#include <unity.h>

#include "on9kvdb.hpp"

using open_signature = esp_err_t (on9kvdb::*)(on9kvdb_bytes, on9kvdb_open_mode, on9kvdb_handle *);
using set_signature = esp_err_t (on9kvdb::*)(on9kvdb_transaction_handle, on9kvdb_bytes, const uint8_t *, uint32_t);
using read_into_signature = esp_err_t (on9kvdb::*)(on9kvdb_value_reader, void *, uint32_t, uint32_t *);
using begin_writer_signature =
    esp_err_t (on9kvdb::*)(on9kvdb_transaction_handle, on9kvdb_bytes, uint32_t, on9kvdb_value_writer *);
using write_writer_signature = esp_err_t (on9kvdb::*)(on9kvdb_value_writer, const uint8_t *, uint32_t);

static_assert(std::is_same_v<decltype(static_cast<open_signature>(&on9kvdb::open)), open_signature>);
static_assert(std::is_same_v<decltype(&on9kvdb::set), set_signature>);
static_assert(std::is_same_v<decltype(&on9kvdb::read_value_into), read_into_signature>);
static_assert(std::is_same_v<decltype(&on9kvdb::begin_value_write), begin_writer_signature>);
static_assert(std::is_same_v<decltype(&on9kvdb::write_value), write_writer_signature>);

TEST_CASE("on9kvdb vNext accepts binary names", "[on9kvdb]")
{
    const uint8_t name[] = {'n', 0, 's'};
    TEST_ASSERT_TRUE(on9kvdb_def::validate_bytes(name, sizeof(name)));
    TEST_ASSERT_FALSE(on9kvdb_def::validate_bytes(nullptr, sizeof(name)));
    TEST_ASSERT_FALSE(on9kvdb_def::validate_bytes(name, 0));
}
