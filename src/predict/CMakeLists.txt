add_library(zet_predict STATIC
    engine.cpp
    overlay.cpp
)
add_library(zet::predict ALIAS zet_predict)

target_include_directories(zet_predict PUBLIC include)
target_link_libraries(zet_predict PRIVATE zet::core_options)
target_link_libraries(zet_predict PUBLIC zet::grid zet::core zet::compile_options)
