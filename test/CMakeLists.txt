idf_component_register(
        SRCS
            "test_on9kvdb_phase1.cpp"

        PRIV_REQUIRES
            "on9kvdb"
            "unity"

        WHOLE_ARCHIVE
)
