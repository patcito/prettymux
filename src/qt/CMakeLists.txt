cmake_minimum_required(VERSION 3.16)
project(prettymux LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets OpenGLWidgets)

# Ghostty
set(GHOSTTY_DIR "/home/pe/newnewrepos/w/yo/ghostty" CACHE PATH "Path to ghostty source directory")
set(GHOSTTY_LIB_DIR "${GHOSTTY_DIR}/zig-out/lib")
set(GHOSTTY_INCLUDE_DIR "${GHOSTTY_DIR}/include")
set(GLAD_DIR "${GHOSTTY_DIR}/vendor/glad")

set(PRETTYMUX_SOURCES
    main.cpp
    ${GLAD_DIR}/src/gl.c
)

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

target_link_directories(prettymux PRIVATE ${GHOSTTY_LIB_DIR})

target_link_libraries(prettymux PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::OpenGLWidgets
    ghostty
)

# Platform-specific frameworks
if(APPLE)
    find_library(METAL_FRAMEWORK Metal)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore)
    target_link_libraries(prettymux PRIVATE
        ${COCOA_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
    )
    if(METAL_FRAMEWORK)
        target_link_libraries(prettymux PRIVATE ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    endif()
endif()

# RPATH (Linux/macOS)
if(NOT WIN32)
    set_target_properties(prettymux PROPERTIES
        BUILD_RPATH "${GHOSTTY_LIB_DIR}"
        INSTALL_RPATH "${GHOSTTY_LIB_DIR}"
    )
endif()
