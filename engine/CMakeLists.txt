add_library(weft_engine STATIC
    src/devices/NullDevice.cpp
    src/rt/AudioThread.cpp
)
add_library(weft::engine ALIAS weft_engine)
target_include_directories(weft_engine PUBLIC include)
target_link_libraries(weft_engine PUBLIC weft::core)
weft_set_warnings(weft_engine)
