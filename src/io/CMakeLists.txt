add_library(zet_io STATIC
    clock.cpp
    errno.cpp
    fd.cpp
)
add_library(zet::io ALIAS zet_io)

target_include_directories(zet_io PUBLIC include)
# Exceptions are off here too (design.md §10): the layer reports through
# IoResult. It is not part of the no-abort gate, which covers what parses
# data from the network; zet_io only moves bytes.
target_link_libraries(zet_io PRIVATE zet::core_options)
target_link_libraries(zet_io PUBLIC zet::core zet::compile_options)
