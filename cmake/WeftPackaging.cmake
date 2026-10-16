# Install layout and installer (D-ARCH-51). `cmake --install` lays out the app; `cpack` builds a ZIP
# everywhere and, on Windows with NSIS installed, a setup .exe with Start Menu and desktop shortcuts.

if(WIN32)
    set(weft_bin_dir ".")
    set(weft_doc_dir ".")
else()
    set(weft_bin_dir "bin")
    set(weft_doc_dir "share/doc/weft-studio")
endif()

# Everything the app ships is in the "app" component; fetched dependencies' own install rules
# (headers, CMake packages) are left out of the package.
# The scanner must sit next to the app (F-PLG-2).
install(TARGETS WeftStudio weft-plugin-scanner RUNTIME DESTINATION "${weft_bin_dir}" COMPONENT app)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md"
              "${PROJECT_BINARY_DIR}/app/ThirdPartyNotices.txt"
        DESTINATION "${weft_doc_dir}" COMPONENT app)
get_target_property(weft_exe WeftStudio OUTPUT_NAME) # "Weft Studio"

# The Visual C++ runtime goes beside the app, so a fresh Windows install needs nothing else.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${weft_bin_dir}")
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT app)
    include(InstallRequiredSystemLibraries)
endif()

set(CPACK_PACKAGE_NAME "WeftStudio")
set(CPACK_PACKAGE_VENDOR "Weft Studio contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Weft Studio: a music studio for recording, arranging and mixing together")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Weft Studio")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ijlogan1999/Weft-Studio")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_EXECUTABLES "${weft_exe}" "Weft Studio")
set(CPACK_INSTALL_CMAKE_PROJECTS "${PROJECT_BINARY_DIR};WeftStudio;app;/")
set(CPACK_STRIP_FILES TRUE)

set(weft_generators ZIP)
if(WIN32)
    find_program(WEFT_MAKENSIS makensis PATHS "$ENV{ProgramFiles\(x86\)}/NSIS" "$ENV{ProgramFiles}/NSIS")
    if(WEFT_MAKENSIS)
        list(APPEND weft_generators NSIS)
    endif()
    set(CPACK_NSIS_PACKAGE_NAME "Weft Studio")
    set(CPACK_NSIS_DISPLAY_NAME "Weft Studio")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "${weft_exe}.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "${weft_exe}.exe")
    set(CPACK_CREATE_DESKTOP_LINKS "${weft_exe}")
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
    set(CPACK_SYSTEM_NAME "windows-x64")
endif()
set(CPACK_GENERATOR "${weft_generators}")
include(CPack)
