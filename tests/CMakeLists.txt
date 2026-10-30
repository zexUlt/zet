include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
    GIT_SHALLOW TRUE
    SYSTEM
)
# Prevent doctest from installing its headers and CMake files into our prefix.
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

add_executable(zet_tests
    main.cpp
    grid/grid_test.cpp
    core/byte_reader_test.cpp
    core/byte_writer_test.cpp
    core/log_test.cpp
    core/secret_test.cpp
    core/shell_quote_test.cpp
    core/time_test.cpp
    core/version_test.cpp
    crypto/crypto_test.cpp
    session/agent_session_test.cpp
    session/bootstrap_test.cpp
    session/client_session_test.cpp
    session/connection_test.cpp
    session/error_test.cpp
    session/mux_router_test.cpp
    session/send_buffer_test.cpp
    session/stream_state_test.cpp
    session/receive_cursor_test.cpp
    session/reconnect_property_test.cpp
    session/channel_test.cpp
    wire/armor_test.cpp
    wire/probe_test.cpp
    wire/frame_test.cpp
    wire/handshake_test.cpp
    wire/frame_stream_test.cpp
    wire/key_schedule_test.cpp
    wire/message_test.cpp
    wire/mux_message_test.cpp
    wire/sealed_frame_test.cpp
)

target_link_libraries(zet_tests PRIVATE
    zet::core
    zet::crypto
    zet::wire
    zet::session
    zet::grid
    zet::compile_options
    doctest::doctest
)

# zet_io is Linux-only for now (src/CMakeLists.txt), and so are its tests.
# zet_io's portable part and the client on Linux and macOS; the pty, Spawn
# and Detach, which only the agent needs, on Linux (src/CMakeLists.txt).
if(ZET_HAVE_IO)
    target_sources(zet_tests PRIVATE
        client/deliver_test.cpp
        client/escape_test.cpp
        io/clock_test.cpp
        io/fd_test.cpp
        io/poller_test.cpp
        io/self_path_test.cpp
        io/tcp_test.cpp
    )
    # The Waker test wakes the poller from a second thread.
    find_package(Threads REQUIRED)
    target_link_libraries(zet_tests PRIVATE zet::io zet_client_loop Threads::Threads)
endif()

if(ZET_HAVE_AGENT)
    target_sources(zet_tests PRIVATE
        io/activation_test.cpp
        io/daemon_test.cpp
        io/fake_pty.cpp
        io/pty_burst_test.cpp
        io/pty_contract_test.cpp
        io/pty_test.cpp
        io/seccomp_test.cpp
        io/spawn_test.cpp
        io/terminal_test.cpp
        io/unix_test.cpp
    )

    # The child the pty contract and the burst run against (design.md §16):
    # its behaviour on a terminal is known exactly, which a shell's is not.
    add_executable(zet_pty_echo io/pty_echo_child.cpp)
    target_link_libraries(zet_pty_echo PRIVATE zet::compile_options)
    add_dependencies(zet_tests zet_pty_echo)
    target_compile_definitions(zet_tests PRIVATE
        ZET_PTY_ECHO_PATH="$<TARGET_FILE:zet_pty_echo>"
    )
endif()

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(zet_tests)

# Differential-test helper: reads raw bytes from stdin, renders through
# VtParser+Grid, outputs the visible screen as JSON on stdout.
add_executable(zet_grid_dump grid/grid_dump_main.cpp)
target_link_libraries(zet_grid_dump PRIVATE zet::grid zet::compile_options)

add_subdirectory(compile_fail)
