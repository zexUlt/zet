# Install rules for zet. Included from the top-level CMakeLists.txt after
# add_subdirectory(src), so executable targets are visible.
#
# Executable rules are guarded by if(TARGET …): the agent and mux are
# Linux-only (src/CMakeLists.txt), and the client is not built when
# ZET_HAVE_IO is off (bare environments without I/O support).
#
# Chosen layout (CMAKE_INSTALL_PREFIX is /usr/local by default):
#
#   bin/zet                               client (Linux + macOS)
#   bin/zet-agent                         agent, local copy   (Linux)
#   bin/zet-muxd                          mux                 (Linux)
#   libexec/zet/zet-agent-<arch>          agent keyed by arch (Linux)
#     — M5.5 delivery reads this path; the client tarball carries both
#       x86_64 and aarch64 so that connecting to either server works.
#   share/zet/units/zet-muxd.{socket,service}
#     — kept in share/ so any prefix works; a distro package copies or
#       symlinks to /usr/lib/systemd/system/.
#   share/zet/LICENSE  NOTICE  THIRD_PARTY_LICENSES

include(GNUInstallDirs)

# --- Data files (always) ---------------------------------------------------

install(FILES LICENSE NOTICE THIRD_PARTY_LICENSES
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zet)

# systemd units are Linux-only; the macOS client has no use for them.
if(NOT APPLE)
    install(DIRECTORY units/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zet/units
        FILES_MATCHING
            PATTERN "*.service"
            PATTERN "*.socket")
endif()

# --- Executables (guarded by platform) -------------------------------------

if(TARGET zet)
    install(TARGETS zet
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# zet-agent is installed twice:
#   - bin/zet-agent: the locally executable copy.
#   - libexec/zet/zet-agent-<arch>: the delivery-layout copy that M5.5's
#     installer reads when packaging a client tarball. The tarball carries
#     both x86_64 and aarch64 so that the client can bootstrap either.
#     CMAKE_SYSTEM_PROCESSOR is normalised by CMake (x86_64, aarch64).
if(TARGET zet-agent)
    install(TARGETS zet-agent
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS "$<TARGET_FILE:zet-agent>"
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/zet"
        RENAME "zet-agent-${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(TARGET zet-muxd)
    install(TARGETS zet-muxd
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
