cmake_minimum_required(VERSION 3.22)

project(on9kvdb_tests LANGUAGES CXX)

enable_testing()

add_executable(on9kvdb_phase1_tests
        "on9kvdb_phase1_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_phase1_tests PRIVATE "..")
target_compile_features(on9kvdb_phase1_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_phase1_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_phase1 COMMAND on9kvdb_phase1_tests)

add_executable(on9kvdb_phase2_tests
        "on9kvdb_phase2_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_phase2_tests PRIVATE "..")
target_compile_features(on9kvdb_phase2_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_phase2_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_phase2 COMMAND on9kvdb_phase2_tests)

add_executable(on9kvdb_phase3_tests
        "on9kvdb_phase3_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_phase3_tests PRIVATE "..")
target_compile_features(on9kvdb_phase3_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_phase3_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_phase3 COMMAND on9kvdb_phase3_tests)

add_executable(on9kvdb_phase4_tests
        "on9kvdb_phase4_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_phase4_tests PRIVATE "..")
target_compile_features(on9kvdb_phase4_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_phase4_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_phase4 COMMAND on9kvdb_phase4_tests)

add_executable(on9kvdb_phase5_tests
        "on9kvdb_phase5_tests.cpp"
        "../on9kvdb_format.cpp"
)

target_include_directories(on9kvdb_phase5_tests PRIVATE "..")
target_compile_features(on9kvdb_phase5_tests PRIVATE cxx_std_17)
target_compile_options(on9kvdb_phase5_tests PRIVATE
        "-Wall"
        "-Wextra"
        "-Werror"
        "-fno-exceptions"
        "-fno-rtti"
)

add_test(NAME on9kvdb_phase5 COMMAND on9kvdb_phase5_tests)
