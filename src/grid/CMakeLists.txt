add_library(zet_grid STATIC
    grid.cpp
    unicode_tables.cpp
    vt_parser.cpp
)
add_library(zet::grid ALIAS zet_grid)

target_include_directories(zet_grid PUBLIC include)
target_link_libraries(zet_grid PRIVATE zet::core_options)
target_link_libraries(zet_grid PUBLIC zet::compile_options)
