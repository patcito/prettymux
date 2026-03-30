cmake_minimum_required(VERSION 3.16)
project(prettymux LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

if(APPLE)
    enable_language(OBJCXX)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network)
if(NOT APPLE AND NOT WIN32)
    find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)
endif()

# Ghostty
set(GHOSTTY_DIR "/home/pe/newnewrepos/w/yo/ghostty" CACHE PATH "Path to ghostty source directory")
set(GHOSTTY_LIB_DIR "${GHOSTTY_DIR}/zig-out/lib")
set(GHOSTTY_INCLUDE_DIR "${GHOSTTY_DIR}/include")
set(GLAD_DIR "${GHOSTTY_DIR}/vendor/glad")
set(GHOSTTY_MACOS_XCFRAMEWORK_CANDIDATES
    "${GHOSTTY_DIR}/macos/GhosttyKit.xcframework"
    "${GHOSTTY_DIR}/zig-out/macos/GhosttyKit.xcframework"
)

if(APPLE)
    set(GHOSTTY_MACOS_XCFRAMEWORK "")
    foreach(candidate IN LISTS GHOSTTY_MACOS_XCFRAMEWORK_CANDIDATES)
        if(EXISTS "${candidate}")
            set(GHOSTTY_MACOS_XCFRAMEWORK "${candidate}")
            break()
        endif()
    endforeach()
    if(NOT GHOSTTY_MACOS_XCFRAMEWORK)
        list(GET GHOSTTY_MACOS_XCFRAMEWORK_CANDIDATES 0 GHOSTTY_MACOS_XCFRAMEWORK)
    endif()

    find_library(GHOSTTY_MACOS_LIB
        NAMES ghostty libghostty.a
        PATHS
            "${GHOSTTY_MACOS_XCFRAMEWORK}/macos-arm64_x86_64"
            "${GHOSTTY_MACOS_XCFRAMEWORK}/macos-arm64"
        NO_DEFAULT_PATH
    )
    if(NOT GHOSTTY_MACOS_LIB)
        message(FATAL_ERROR "Ghostty macOS static library not found inside ${GHOSTTY_MACOS_XCFRAMEWORK}")
    endif()
endif()

set(PRETTYMUX_SOURCES
    main.cpp
    browser_view.h
    ${GLAD_DIR}/src/gl.c
)

if(APPLE)
    list(APPEND PRETTYMUX_SOURCES browser_view_macos.mm)
elseif(WIN32)
    list(APPEND PRETTYMUX_SOURCES
        browser_view_windows.cpp
        ghostty_windows_stub.c
    )
else()
    list(APPEND PRETTYMUX_SOURCES browser_view_qwebengine.cpp)
endif()

# Windows resource file for icon
if(WIN32)
    list(APPEND PRETTYMUX_SOURCES ${CMAKE_SOURCE_DIR}/../../packaging/prettymux.rc)
endif()

if(APPLE)
    add_executable(prettymux MACOSX_BUNDLE ${PRETTYMUX_SOURCES})
    set_target_properties(prettymux PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "PrettyMux"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.prettymux.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
    )
elseif(WIN32)
    add_executable(prettymux WIN32 ${PRETTYMUX_SOURCES})
else()
    add_executable(prettymux ${PRETTYMUX_SOURCES})
endif()

target_include_directories(prettymux PRIVATE
    ${GHOSTTY_INCLUDE_DIR}
    ${GLAD_DIR}/include
)

if(NOT APPLE AND NOT WIN32)
    target_link_directories(prettymux PRIVATE ${GHOSTTY_LIB_DIR})
endif()

target_link_libraries(prettymux PRIVATE
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Network
)

if(NOT WIN32)
    target_link_libraries(prettymux PRIVATE ghostty)
endif()

if(NOT APPLE AND NOT WIN32)
    target_link_libraries(prettymux PRIVATE Qt6::WebEngineWidgets)
endif()

if(APPLE)
    set_source_files_properties(browser_view_macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

# Platform-specific frameworks
if(APPLE)
    find_library(METAL_FRAMEWORK Metal)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)
    find_library(WEBKIT_FRAMEWORK WebKit)
    target_link_libraries(prettymux PRIVATE
        ${GHOSTTY_MACOS_LIB}
        ${COCOA_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${WEBKIT_FRAMEWORK}
    )
    if(METAL_FRAMEWORK)
        target_link_libraries(prettymux PRIVATE ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()
endif()

if(WIN32)
    set(WEBVIEW2_DIR "" CACHE PATH "Path to the Microsoft.Web.WebView2 NuGet package root")
    if(NOT WEBVIEW2_DIR)
        message(FATAL_ERROR "WEBVIEW2_DIR must point to the Microsoft.Web.WebView2 package directory")
    endif()

    target_include_directories(prettymux PRIVATE "${WEBVIEW2_DIR}/build/native/include")
    target_link_libraries(prettymux PRIVATE
        "${WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib"
        ole32
        user32
    )
endif()

# RPATH (Linux/macOS)
if(NOT WIN32 AND NOT APPLE)
    set_target_properties(prettymux PROPERTIES
        BUILD_RPATH "${GHOSTTY_LIB_DIR}"
        INSTALL_RPATH "${GHOSTTY_LIB_DIR}"
    )
endif()
