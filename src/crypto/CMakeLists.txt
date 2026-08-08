find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

add_library(zet_crypto STATIC
    crypto.cpp
)
add_library(zet::crypto ALIAS zet_crypto)

target_include_directories(zet_crypto PUBLIC include)
target_link_libraries(zet_crypto PRIVATE zet::core_options)
target_link_libraries(zet_crypto PUBLIC zet::core zet::compile_options PkgConfig::sodium)

# Invariant 6a. The libsodium documentation forbids LTO outright: it can turn a
# constant-time comparison into a branch. Neither the sanitisers nor the other
# gates catch that, so the only defence is to never switch it on.
set_target_properties(zet_crypto PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
