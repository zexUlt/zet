# zet-muxd (design.md §3, §6): the loop in a library, the binary a main().
add_library(zet_mux_loop STATIC
    mux.cpp
)
target_link_libraries(zet_mux_loop PRIVATE zet::core_options)
target_link_libraries(zet_mux_loop PUBLIC
    zet::app zet::session zet::crypto zet::io zet::compile_options)

add_executable(zet-muxd main.cpp)
target_link_libraries(zet-muxd PRIVATE zet_mux_loop zet::core_options)
