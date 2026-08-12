idf_component_register(
        SRCS
            "on9kvdb.cpp"
            "on9kvdb_api.cpp"
            "on9kvdb_compaction.cpp"
            "on9kvdb_format.cpp"
            "on9kvdb_io.cpp"
            "on9kvdb_io_sdmmc.cpp"
            "on9kvdb_manifest.cpp"
            "on9kvdb_memtable.cpp"
            "on9kvdb_table.cpp"
            "on9kvdb_value.cpp"
            "on9kvdb_wal.cpp"

        INCLUDE_DIRS
            "."

        REQUIRES
            "esp_common"
            "esp_system"
            "esp_timer"
            "esp_driver_sdmmc"
            "esp_psram"
            "freertos"
            "heap"
            "log"
            "sdmmc"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
        "-fno-exceptions"
        "-fno-rtti"
)
