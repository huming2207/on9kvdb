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
