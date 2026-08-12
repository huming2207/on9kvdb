cmake_minimum_required(VERSION 3.22)

project(on9kvdb_tests LANGUAGES CXX)

enable_testing()

add_executable(on9kvdb_vnext_tests
        "on9kvdb_vnext_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_vnext_tests PRIVATE "..")
target_compile_features(on9kvdb_vnext_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_vnext_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_vnext COMMAND on9kvdb_vnext_tests)

add_executable(on9kvdb_core_tests
        "on9kvdb_core_tests.cpp"
        "../on9kvdb.cpp"
        "../on9kvdb_api.cpp"
        "../on9kvdb_compaction.cpp"
        "../on9kvdb_format.cpp"
        "../on9kvdb_io.cpp"
        "../on9kvdb_manifest.cpp"
        "../on9kvdb_memtable.cpp"
        "../on9kvdb_table.cpp"
        "../on9kvdb_value.cpp"
        "../on9kvdb_wal.cpp"
)

target_include_directories(on9kvdb_core_tests PRIVATE "host_stubs" "..")
target_compile_features(on9kvdb_core_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_core_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_core COMMAND on9kvdb_core_tests)
