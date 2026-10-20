# The portable part, POSIX on every platform the client runs on, and the
# Poller backend picked by platform (design.md §10): one header, one .cpp per
# backend, no runtime choice. pty, Spawn and Detach are the agent's, and the
# agent is Linux-only (§12).
add_library(zet_io STATIC
    clock.cpp
    errno.cpp
    fd.cpp
    poller.cpp
    tcp.cpp
    terminal.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(zet_io PRIVATE
        activation.cpp
        daemon.cpp
        poller_epoll.cpp
        pty.cpp
        seccomp.cpp
        spawn.cpp
        unix.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(zet_io PRIVATE
        poller_kqueue.cpp
    )
else()
    message(FATAL_ERROR "zet_io has no Poller backend for ${CMAKE_SYSTEM_NAME}")
endif()
add_library(zet::io ALIAS zet_io)

target_include_directories(zet_io PUBLIC include)
# Exceptions are off here too (design.md §10): the layer reports through
# IoResult. It is not part of the no-abort gate, which covers what parses
# data from the network; zet_io only moves bytes.
target_link_libraries(zet_io PRIVATE zet::core_options)
target_link_libraries(zet_io PUBLIC zet::core zet::compile_options)
