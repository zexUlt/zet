# zet-agent (design.md §3): the loop in a library of its own, so that the
# binary is one main() and nothing else.
add_library(zet_agent_loop STATIC
    agent.cpp
    mux_link.cpp
)
target_link_libraries(zet_agent_loop PRIVATE zet::core_options)
target_link_libraries(zet_agent_loop PUBLIC
    zet::app zet::session zet::crypto zet::io zet::compile_options)

add_executable(zet-agent main.cpp)
target_link_libraries(zet-agent PRIVATE zet_agent_loop zet::core_options)
