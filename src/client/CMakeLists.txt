# zet, the client (design.md §3): the loop in a library, the binary a main().
add_library(zet_client_loop STATIC
    client.cpp
    escape.cpp
)
target_include_directories(zet_client_loop PUBLIC .)
target_link_libraries(zet_client_loop PRIVATE zet::core_options)
target_link_libraries(zet_client_loop PUBLIC
    zet::app zet::session zet::crypto zet::io zet::compile_options)

add_executable(zet main.cpp)
target_link_libraries(zet PRIVATE zet_client_loop zet::core_options)
