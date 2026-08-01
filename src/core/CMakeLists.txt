# Explicit file lists, never file(GLOB): a glob turns a typo into a file that
# silently is not built. EternalTerminal lost a whole test that way for years.
add_library(zet_core STATIC
    byte_reader.cpp
    byte_writer.cpp
)
add_library(zet::core ALIAS zet_core)

target_include_directories(zet_core PUBLIC include)
target_link_libraries(zet_core PRIVATE zet::core_options)
target_link_libraries(zet_core PUBLIC zet::compile_options)
