add_library(zet_session STATIC
    agent_session.cpp
    client_session.cpp
    connection.cpp
    error.cpp
    send_buffer.cpp
    stream_state.cpp
    receive_cursor.cpp
    channel.cpp
)
add_library(zet::session ALIAS zet_session)

target_include_directories(zet_session PUBLIC include)
target_link_libraries(zet_session PRIVATE zet::core_options)
target_link_libraries(zet_session PUBLIC zet::wire zet::compile_options)
