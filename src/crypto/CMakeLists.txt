find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

add_library(zet_crypto STATIC
    crypto.cpp
)
add_library(zet::crypto ALIAS zet_crypto)

target_include_directories(zet_crypto PUBLIC include)
target_link_libraries(zet_crypto PRIVATE zet::core_options)
target_link_libraries(zet_crypto PUBLIC zet::core zet::compile_options PkgConfig::sodium)

# Инвариант 6a. Документация libsodium запрещает LTO прямо: она способна
# превратить константное по времени сравнение в ветвление. Санитайзеры и
# остальные гейты такого не ловят, поэтому единственная защита — не включать.
set_target_properties(zet_crypto PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
