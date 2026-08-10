add_library(zet_wire STATIC
    frame.cpp
    frame_stream.cpp
    key_schedule.cpp
    message.cpp
    sealed_frame.cpp
)
add_library(zet::wire ALIAS zet_wire)

target_include_directories(zet_wire PUBLIC include)
target_link_libraries(zet_wire PRIVATE zet::core_options)
target_link_libraries(zet_wire PUBLIC zet::core zet::crypto zet::compile_options)
