include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(doctest)

add_executable(zet_tests
    main.cpp
    core/byte_reader_test.cpp
    core/byte_writer_test.cpp
    core/log_test.cpp
    core/secret_test.cpp
    core/time_test.cpp
    crypto/crypto_test.cpp
    wire/frame_test.cpp
    wire/sealed_frame_test.cpp
)

target_link_libraries(zet_tests PRIVATE
    zet::core
    zet::crypto
    zet::wire
    zet::compile_options
    doctest::doctest
)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(zet_tests)
