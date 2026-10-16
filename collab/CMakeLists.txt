# Client side of collaboration (spec 05): relay accounts and projects, and the sync
# client that keeps a Session in step with the relay. No JUCE, so weft-cli and the
# [S] tests use it too.
add_library(weft_collab STATIC
    src/Relay.cpp
    src/SyncClient.cpp
)
add_library(weft::collab ALIAS weft_collab)
target_include_directories(weft_collab PUBLIC include)
target_link_libraries(weft_collab PUBLIC weft::core PRIVATE weft::sync)
weft_set_warnings(weft_collab)
