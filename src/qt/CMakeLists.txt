cmake_minimum_required(VERSION 3.16)
project(prettymux LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets OpenGLWidgets)

# Ghostty
set(GHOSTTY_DIR "/home/pe/newnewrepos/w/yo/ghostty")
set(GHOSTTY_LIB_DIR "${GHOSTTY_DIR}/zig-out/lib")
set(GHOSTTY_INCLUDE_DIR "${GHOSTTY_DIR}/include")
set(GLAD_DIR "${GHOSTTY_DIR}/vendor/glad")

add_executable(prettymux
    main.cpp
    ${GLAD_DIR}/src/gl.c
)

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

# RPATH
set_target_properties(prettymux PROPERTIES
    BUILD_RPATH "${GHOSTTY_LIB_DIR}"
    INSTALL_RPATH "${GHOSTTY_LIB_DIR}"
)
