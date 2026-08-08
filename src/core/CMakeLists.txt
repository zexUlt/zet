# configure_file rather than a -D: a header is readable, greppable and works
# the same for anything that includes it, while a definition on one target is
# absent everywhere it was not repeated.
configure_file(
    include/zet/core/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/zet/core/version.hpp
    @ONLY
)

add_library(zet_core STATIC
    byte_reader.cpp
    byte_writer.cpp
    log.cpp
    secure_zero.cpp
)
add_library(zet::core ALIAS zet_core)

target_include_directories(zet_core PUBLIC
    include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
target_link_libraries(zet_core PRIVATE zet::core_options)
target_link_libraries(zet_core PUBLIC zet::compile_options)
