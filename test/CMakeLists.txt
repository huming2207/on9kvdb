idf_component_register(
        SRCS
            "test_on9kvdb_vnext.cpp"

        PRIV_REQUIRES
            "on9kvdb"
            "unity"

        WHOLE_ARCHIVE
)
