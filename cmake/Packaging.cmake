# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Oscilline contributors
#
# Icons, install rules, and CPack settings for the oscilline game. The
# developer tools (viewer, extract, chart, inspect) are not installed.
#
# OSCILLINE_PACKAGING builds a Windows GUI executable (no console window) and a
# macOS app bundle. Leave it off for development: command-line output then
# stays visible on Windows and the binary stays a plain file on macOS.

include(GNUInstallDirs)

set(OSCILLINE_APP_ID "io.github.marioaddict3.Oscilline")
set(OSCILLINE_PACKAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging")

if(WIN32)
    set(OSCILLINE_ICON_ICO "${OSCILLINE_PACKAGING_DIR}/icons/oscilline.ico")
    configure_file("${OSCILLINE_PACKAGING_DIR}/windows/oscilline.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/generated/oscilline.rc" @ONLY)
    target_sources(oscilline PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/oscilline.rc")
    if(OSCILLINE_PACKAGING)
        set_target_properties(oscilline PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    install(TARGETS oscilline RUNTIME DESTINATION .)
    install(FILES LICENSE DESTINATION . RENAME LICENSE.txt)
    install(FILES README.md DESTINATION .)
elseif(APPLE AND OSCILLINE_PACKAGING)
    # Set CMAKE_OSX_DEPLOYMENT_TARGET when configuring to build for older macOS.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(OSCILLINE_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(OSCILLINE_MACOS_MINIMUM "11.0")
    endif()
    set(icns "${OSCILLINE_PACKAGING_DIR}/icons/oscilline.icns")
    target_sources(oscilline PRIVATE "${icns}")
    set_source_files_properties("${icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(oscilline PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME oscilline
        MACOSX_BUNDLE_INFO_PLIST "${OSCILLINE_PACKAGING_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    install(TARGETS oscilline BUNDLE DESTINATION .)
    install(FILES LICENSE README.md DESTINATION .)
else()
    # Linux and other Unix: the freedesktop layout used by AppImage and Flatpak.
    install(TARGETS oscilline RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${OSCILLINE_PACKAGING_DIR}/linux/${OSCILLINE_APP_ID}.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${OSCILLINE_PACKAGING_DIR}/linux/${OSCILLINE_APP_ID}.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    install(FILES "${OSCILLINE_PACKAGING_DIR}/icons/${OSCILLINE_APP_ID}.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${OSCILLINE_PACKAGING_DIR}/icons/png/${size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
                RENAME "${OSCILLINE_APP_ID}.png")
    endforeach()
    install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
endif()

set(CPACK_PACKAGE_NAME "Oscilline")
set(CPACK_PACKAGE_VENDOR "Oscilline contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Rhythm runs on a vector ribbon")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Oscilline")
else()
    set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)
