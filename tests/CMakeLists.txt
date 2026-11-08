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
    grid/grid_fidelity_test.cpp
    grid/probe_test.cpp
    grid/snapshot_test.cpp
    grid/snapshot_roundtrip_test.cpp
    grid/snapshot_restore_test.cpp
    grid/snapshot_ladder_test.cpp
    grid/snapshot_size_test.cpp
    grid/snapshot_400x300_test.cpp
    grid/snapshot_mutant_test.cpp
    predict/predict_ack_test.cpp
    predict/predict_backspace_test.cpp
    predict/predict_boundary_test.cpp
    predict/predict_cap_test.cpp
    predict/predict_column_trust_test.cpp
    predict/grid_counters_test.cpp
    predict/predict_epoch_test.cpp
    predict/predict_input_test.cpp
    predict/predict_leak_test.cpp
    predict/predict_leak_screen_test.cpp
    predict/predict_password_test.cpp
    predict/predict_tentative_test.cpp
    predict/predict_trigger_test.cpp
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
    session/deferred_input_ack_test.cpp
    session/agent_input_overrun_test.cpp
    session/channel_read_gate_test.cpp
    session/channel_send_gate_test.cpp
    session/grid_violation_test.cpp
    session/grid_path_property_test.cpp
    session/grid_fragmentation_test.cpp
    session/stale_stream_gen_test.cpp
    session/connection_snapshot_probe_test.cpp
    session/resume_resize_same_read_test.cpp
    session/agent_await_snapshot_test.cpp
    session/channel_v1_violation_test.cpp
    session/connection_probe94c_test.cpp
    session/channel_probe94c_test.cpp
    session/echo_ack_hostile_test.cpp
    session/echo_ack_negotiation_test.cpp
    session/echo_history_test.cpp
    session/channel_rtt_test.cpp
    session/agent_echo_ack_test.cpp
    wire/armor_test.cpp
    wire/echo_ack_test.cpp
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
    zet::predict
    zet::compile_options
    doctest::doctest
)

# zet_io is Linux-only for now (src/CMakeLists.txt), and so are its tests.
# zet_io's portable part and the client on Linux and macOS; the pty, Spawn
# and Detach, which only the agent needs, on Linux (src/CMakeLists.txt).
if(ZET_HAVE_IO)
    target_sources(zet_tests PRIVATE
        app/byte_queue_test.cpp
        app/env_test.cpp
        client/deliver_test.cpp
        client/escape_test.cpp
        client/mirror_loop_test.cpp
        client/mirror_test.cpp
        client/read_gate_client_test.cpp
        client/reconnect_backoff_test.cpp
        client/snapshot_client_test.cpp
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
        agent/agent_echo_ack_wiring_test.cpp
        agent/agent_failed_grid_resize_test.cpp
        agent/agent_input_queue_test.cpp
        agent/agent_ground_timeout_test.cpp
        agent/agent_sixel_test.cpp
        agent/agent_replay_pacing_test.cpp
        agent/agent_wiring_test.cpp
        agent/client_input_replay_pacing_test.cpp
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
    target_link_libraries(zet_tests PRIVATE zet_agent_loop)
endif()

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(zet_tests)

# Size-measurement test gets the corpus directory at compile time so it can
# read the .bin files directly without a separate fixture.
target_compile_definitions(zet_tests PRIVATE
    ZET_GRID_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/integration/grid_corpus"
)

# Differential-test helper: reads raw bytes from stdin, renders through
# VtParser+Grid, outputs the visible screen as JSON on stdout.
add_executable(zet_grid_dump grid/grid_dump_main.cpp)
target_link_libraries(zet_grid_dump PRIVATE zet::grid zet::compile_options)

# Snapshot differential-test helper: reads raw bytes from stdin, renders
# through VtParser+Grid, serialises via Snapshot::Create, writes snapshot bytes
# raw to stdout.  Used by tests/integration/test_snapshot_diff.py.
add_executable(zet_snapshot_dump grid/snapshot_dump_main.cpp)
target_link_libraries(zet_snapshot_dump PRIVATE zet::grid zet::compile_options)

add_subdirectory(compile_fail)
