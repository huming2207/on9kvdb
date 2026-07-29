idf_component_register(
        SRCS
            "on9kvdb.cpp"
            "on9kvdb_format.cpp"
            "on9kvdb_io.cpp"
            "on9kvdb_manifest.cpp"

        INCLUDE_DIRS
            "."

        REQUIRES
            "esp_common"
            "esp_system"
            "fatfs"
            "freertos"
            "log"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
        "-fno-exceptions"
        "-fno-rtti"
)
