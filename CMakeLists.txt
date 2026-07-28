idf_component_register(
        SRCS
            "on9kvdb.cpp"
            "on9kvdb_format.cpp"

        INCLUDE_DIRS
            "."

        REQUIRES
            "esp_common"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
        "-fno-exceptions"
        "-fno-rtti"
)
