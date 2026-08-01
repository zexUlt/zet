# Warning set and hardening applied to every zet target.
#
# -Wshadow is not optional here: a local shadowing a member is a silent
# functional break, not a style question. EternalTerminal shipped exactly that
# bug for years (PseudoUserTerminal.hpp: a local `pid` from forkpty() shadowed
# the member, so waitid() ran on garbage).

add_library(zet_compile_options INTERFACE)
add_library(zet::compile_options ALIAS zet_compile_options)

target_compile_options(zet_compile_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wcast-qual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wundef
)

if(ZET_WERROR)
    target_compile_options(zet_compile_options INTERFACE -Werror)
endif()

# Bounds checks in the standard library. Cheap, and the whole point is that a
# violation is caught rather than becoming a silent read of someone's memory.
# Both spellings are set: Clang here links against libstdc++ by default, but
# nothing stops a build from selecting libc++.
target_compile_definitions(zet_compile_options INTERFACE
    $<$<NOT:$<CONFIG:Release>>:_GLIBCXX_ASSERTIONS>
    $<$<NOT:$<CONFIG:Release>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE>
)

# Applied to the protocol core only. These translation units must not be able
# to terminate the process; see tools/check_no_abort.sh, which enforces it at
# link time rather than by convention.
add_library(zet_core_options INTERFACE)
add_library(zet::core_options ALIAS zet_core_options)

target_link_libraries(zet_core_options INTERFACE zet::compile_options)
target_compile_options(zet_core_options INTERFACE -fno-rtti)
