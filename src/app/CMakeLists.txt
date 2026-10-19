# What both binaries share around the loop: the environment, queues for
# descriptors that took less than everything, poller registrations, a log
# sink. Not the protocol core: this is the layer that may allocate.
add_library(zet_app STATIC
    byte_queue.cpp
    env.cpp
    fd_sink.cpp
    registration.cpp
)
add_library(zet::app ALIAS zet_app)

target_include_directories(zet_app PUBLIC include)
target_link_libraries(zet_app PRIVATE zet::core_options)
target_link_libraries(zet_app PUBLIC zet::io zet::core zet::compile_options)
