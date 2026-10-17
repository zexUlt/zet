add_subdirectory(core)
add_subdirectory(crypto)
add_subdirectory(wire)
add_subdirectory(session)

# Linux first (design.md §10): epoll, signalfd, eventfd. On any other system
# zet_io is not built, and neither is anything that needs it; the sans-I/O
# core and its tests still are, which is what a macOS laptop builds natively.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ZET_HAVE_IO ON PARENT_SCOPE)
    add_subdirectory(io)
    # The binaries need zet_io, and with it Linux (§12: the client comes to
    # macOS with the kqueue backend).
    add_subdirectory(app)
    add_subdirectory(agent)
    add_subdirectory(client)
else()
    set(ZET_HAVE_IO OFF PARENT_SCOPE)
endif()
