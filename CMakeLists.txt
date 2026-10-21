cmake_minimum_required(VERSION 3.22)
set(SC_MODULE litehtml)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-litehtml" LANGUAGES C CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_LITEHTML_BUILD_TESTS "Build simply-cpp-litehtml tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::rest fetches what a page links to over HTTP.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.20.0)
# sc::canvas, fonts, bitmaps and drawings: what a page is drawn with.
find_or_install_package(sc-graphics simply-cpp-graphics-dev simply-cpp-graphics 6.1.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

# litehtml (BSD-3-Clause) and the gumbo HTML parser it ships (Apache-2.0), vendored in
# third_party/litehtml as a git subtree of roelofrossouw/litehtml, are compiled into this module's
# libraries; their headers stay out of the installed ones.
set(LITEHTML_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/litehtml)
file(GLOB LITEHTML_SOURCES CONFIGURE_DEPENDS ${LITEHTML_DIR}/src/*.cpp)
file(GLOB GUMBO_SOURCES CONFIGURE_DEPENDS ${LITEHTML_DIR}/src/gumbo/*.c)
set(LITEHTML_INCLUDE_DIRS ${LITEHTML_DIR}/include ${LITEHTML_DIR}/src ${LITEHTML_DIR}/src/gumbo/include)
add_library(sc-litehtml-engine OBJECT ${LITEHTML_SOURCES} ${GUMBO_SOURCES})
# Hidden from other libraries: MuPDF links the system's gumbo, which litehtml's own copy (changed)
# mustn't stand in for, or the other way round, in a program using both.
set_target_properties(sc-litehtml-engine PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99 EXCLUDE_FROM_ALL ON
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(sc-litehtml-engine PRIVATE ${LITEHTML_INCLUDE_DIRS} ${LITEHTML_DIR}/include/litehtml ${LITEHTML_DIR}/src/gumbo/include/gumbo)
target_compile_options(sc-litehtml-engine PRIVATE -w) # upstream's code, upstream's warnings
list(APPEND SOURCE_OBJECTS $<TARGET_OBJECTS:sc-litehtml-engine>)

add_sc_object(html
        SOURCES src/container.cpp src/container.h
        INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src ${LITEHTML_INCLUDE_DIRS}
        LINK_LIBRARIES sc::sc-core sc::sc-graphics
        PUBLIC_LINK_LIBRARIES sc::sc-core sc::sc-graphics)

add_sc_libraries(DESCRIPTION "simply-cpp-litehtml")

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-litehtml
        COMPONENT development)

if (SC_LITEHTML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-litehtml-test)
endif ()

add_sc_example(sc-litehtml-demo)

# sc-graphics' code in this library falls back to an emoji font, which its package brings.
package_sc_module(DEPENDS fonts-noto-color-emoji
        DEV_DEPENDS "simply-cpp-dev (>= 1.20.0~)" "simply-cpp-graphics-dev (>= 6.1.0~)")
