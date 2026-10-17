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
    core/version_test.cpp
    crypto/crypto_test.cpp
    session/agent_session_test.cpp
    session/client_session_test.cpp
    session/connection_test.cpp
    session/error_test.cpp
    session/send_buffer_test.cpp
    session/stream_state_test.cpp
    session/receive_cursor_test.cpp
    session/reconnect_property_test.cpp
    session/channel_test.cpp
    wire/frame_test.cpp
    wire/handshake_test.cpp
    wire/frame_stream_test.cpp
    wire/key_schedule_test.cpp
    wire/message_test.cpp
    wire/sealed_frame_test.cpp
)

target_link_libraries(zet_tests PRIVATE
    zet::core
    zet::crypto
    zet::wire
    zet::session
    zet::compile_options
    doctest::doctest
)

# zet_io is Linux-only for now (src/CMakeLists.txt), and so are its tests.
if(ZET_HAVE_IO)
    target_sources(zet_tests PRIVATE
        io/clock_test.cpp
        io/daemon_test.cpp
        io/fd_test.cpp
        io/poller_test.cpp
        io/pty_test.cpp
        io/spawn_test.cpp
        io/fake_pty.cpp
        io/pty_contract_test.cpp
        io/pty_burst_test.cpp
        io/tcp_test.cpp
        io/terminal_test.cpp
    )

    # The child the pty contract and the burst run against (design.md §16):
    # its behaviour on a terminal is known exactly, which a shell's is not.
    add_executable(zet_pty_echo io/pty_echo_child.cpp)
    target_link_libraries(zet_pty_echo PRIVATE zet::compile_options)
    add_dependencies(zet_tests zet_pty_echo)
    target_compile_definitions(zet_tests PRIVATE
        ZET_PTY_ECHO_PATH="$<TARGET_FILE:zet_pty_echo>"
    )
    # The Waker test wakes the poller from a second thread.
    find_package(Threads REQUIRED)
    target_link_libraries(zet_tests PRIVATE zet::io Threads::Threads)
endif()

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(zet_tests)

add_subdirectory(compile_fail)
