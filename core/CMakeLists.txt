configure_file(src/version.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/version.cpp" @ONLY)

add_library(weft_core STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/version.cpp"
    src/doc/Document.cpp
)
add_library(weft::core ALIAS weft_core)
target_include_directories(weft_core PUBLIC include)
target_link_libraries(weft_core PUBLIC weft::crdt)
weft_set_warnings(weft_core)
