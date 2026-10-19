add_subdirectory(core)
add_subdirectory(crypto)
add_subdirectory(wire)
add_subdirectory(session)

# zet_io and the client build on Linux and macOS (design.md §10, §12): the
# Poller has an epoll and a kqueue backend. The agent, which owns the pty and
# its pathologies, is Linux-only. Elsewhere only the sans-I/O core builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ZET_HAVE_IO ON PARENT_SCOPE)
    add_subdirectory(io)
    add_subdirectory(app)
    add_subdirectory(client)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(ZET_HAVE_AGENT ON PARENT_SCOPE)
        add_subdirectory(agent)
    else()
        set(ZET_HAVE_AGENT OFF PARENT_SCOPE)
    endif()
else()
    set(ZET_HAVE_IO OFF PARENT_SCOPE)
    set(ZET_HAVE_AGENT OFF PARENT_SCOPE)
endif()
