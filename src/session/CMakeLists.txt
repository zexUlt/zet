add_library(zet_session STATIC
    error.cpp
)
add_library(zet::session ALIAS zet_session)

target_include_directories(zet_session PUBLIC include)
target_link_libraries(zet_session PRIVATE zet::core_options)
target_link_libraries(zet_session PUBLIC zet::wire zet::compile_options)
