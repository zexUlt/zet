add_library(zet_core STATIC
    byte_reader.cpp
    byte_writer.cpp
    log.cpp
    secure_zero.cpp
)
add_library(zet::core ALIAS zet_core)

target_include_directories(zet_core PUBLIC include)
target_link_libraries(zet_core PRIVATE zet::core_options)
target_link_libraries(zet_core PUBLIC zet::compile_options)
