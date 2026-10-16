# Fetches C++ dependencies. The only place a dependency can come from is
# third_party/cpp-deps.json, which is also the license manifest (AT-SYS-1).
include(FetchContent)

file(READ "${PROJECT_SOURCE_DIR}/third_party/cpp-deps.json" WEFT_DEPS_JSON)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/third_party/cpp-deps.json")

# weft_fetch(<name>) declares and populates a dependency listed in cpp-deps.json.
# Sets WEFT_SRC_<name> and records the source dir for the license check.
function(weft_fetch name)
    string(JSON entry ERROR_VARIABLE err GET "${WEFT_DEPS_JSON}" "${name}")
    if(err)
        message(FATAL_ERROR "Dependency '${name}' is not in third_party/cpp-deps.json (AT-SYS-1)")
    endif()
    # A git repository at a tag, or an archive pinned by its SHA-512.
    string(JSON git ERROR_VARIABLE no_git GET "${entry}" git)
    if(no_git)
        string(JSON url GET "${entry}" url)
        string(JSON sha512 GET "${entry}" sha512)
        FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA512=${sha512}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE ${ARGN})
    else()
        string(JSON tag GET "${entry}" tag)
        FetchContent_Declare(${name} GIT_REPOSITORY "${git}" GIT_TAG "${tag}" GIT_SHALLOW TRUE ${ARGN})
    endif()
    FetchContent_MakeAvailable(${name})
    string(TOLOWER "${name}" lname)
    set(WEFT_SRC_${name} "${${lname}_SOURCE_DIR}" PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY WEFT_FETCHED_DEPS "${name}=${${lname}_SOURCE_DIR}")
endfunction()
