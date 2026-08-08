find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED libsodium)

# pkg-config is asked for headers and the version, not for what to link: its
# imported target names libsodium.so even with PKG_CONFIG_USE_STATIC_LIBS, and
# lld answers that with "attempted static link of dynamic object". The binaries
# ship statically linked, so the archive is named outright.
find_library(SODIUM_STATIC_LIBRARY
    NAMES libsodium.a
    HINTS ${sodium_LIBRARY_DIRS}
    REQUIRED
)

add_library(zet_crypto STATIC
    crypto.cpp
)
add_library(zet::crypto ALIAS zet_crypto)

target_include_directories(zet_crypto PUBLIC include)
target_include_directories(zet_crypto SYSTEM PUBLIC ${sodium_INCLUDE_DIRS})
target_link_libraries(zet_crypto PRIVATE zet::core_options)
target_link_libraries(zet_crypto PUBLIC zet::core zet::compile_options ${SODIUM_STATIC_LIBRARY})

# Invariant 6a. The libsodium documentation forbids LTO outright: it can turn a
# constant-time comparison into a branch. Neither the sanitisers nor the other
# gates catch that, so the only defence is to never switch it on.
set_target_properties(zet_crypto PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
