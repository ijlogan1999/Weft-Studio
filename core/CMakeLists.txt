configure_file(src/version.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/version.cpp" @ONLY)

add_library(weft_core STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/version.cpp"
    src/commands/Commands.cpp
    src/commands/Session.cpp
    src/commands/Txn.cpp
    src/core/Id.cpp
    src/doc/Document.cpp
    src/doc/Path.cpp
    src/doc/Value.cpp
    src/model/Model.cpp
    src/model/Projector.cpp
    src/project/ProjectFolder.cpp
    src/time/Quantize.cpp
    src/time/SignatureMap.cpp
    src/time/TempoMap.cpp
)
add_library(weft::core ALIAS weft_core)
target_include_directories(weft_core PUBLIC include)
target_link_libraries(weft_core PUBLIC weft::crdt)
weft_set_warnings(weft_core)
