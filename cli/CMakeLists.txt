add_executable(weft-cli main.cpp)
target_link_libraries(weft-cli PRIVATE weft::engine)
weft_set_warnings(weft-cli)
