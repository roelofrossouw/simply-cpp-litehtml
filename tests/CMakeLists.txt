## TESTS ##
# What every test in this directory links against.
set(SC_TEST_LINK_LIBRARIES sc-litehtml)

add_sc_test(html LABELS unit)

# litehtml's own render tests, drawn by sc-litehtml (upstream.cpp): litehtml-tests at the commit our
# litehtml pins (third_party/litehtml/CMakeLists.txt), downloaded once into ~/.cache/simply-cpp.
# Without it (offline), test-upstream says so and passes. It checks the pages in upstream/pages.txt.
set(LITEHTML_TESTS_COMMIT f750c7c4c1f0b2db63049f45ce505442b2685fa6)
set(LITEHTML_TESTS_SHA256 a43a7b499fd6fbb41aa9ac8892db533b524e2de17e8103363dbbe5ae62c50c2b)
if (DEFINED ENV{HOME})
    set(litehtml_tests_cache "$ENV{HOME}/.cache/simply-cpp")
else ()
    set(litehtml_tests_cache "${CMAKE_CURRENT_BINARY_DIR}")
endif ()
set(litehtml_tests_archive "${litehtml_tests_cache}/litehtml-tests-${LITEHTML_TESTS_COMMIT}.tar.gz")
set(litehtml_tests_dir "${CMAKE_CURRENT_BINARY_DIR}/litehtml-tests-${LITEHTML_TESTS_COMMIT}")
if (NOT EXISTS "${litehtml_tests_archive}")
    file(DOWNLOAD "https://github.com/litehtml/litehtml-tests/archive/${LITEHTML_TESTS_COMMIT}.tar.gz" "${litehtml_tests_archive}.part"
            EXPECTED_HASH SHA256=${LITEHTML_TESTS_SHA256} STATUS litehtml_tests_status)
    list(GET litehtml_tests_status 0 litehtml_tests_code)
    if (litehtml_tests_code EQUAL 0)
        file(RENAME "${litehtml_tests_archive}.part" "${litehtml_tests_archive}")
    else ()
        file(REMOVE "${litehtml_tests_archive}.part")
        message(STATUS "litehtml-tests not downloaded (${litehtml_tests_status}): test-upstream will be skipped")
    endif ()
endif ()
if (EXISTS "${litehtml_tests_archive}" AND NOT EXISTS "${litehtml_tests_dir}/render")
    file(ARCHIVE_EXTRACT INPUT "${litehtml_tests_archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif ()

# litehtml-tests' fonts, as its fonts.cpp adds them: the .ttf files in fonts/ itself (fonts/tmp has
# others, among them fonts named "serif" and "sans-serif", that it leaves out), alone, for fontconfig.
set(render_fonts_dir "${CMAKE_CURRENT_BINARY_DIR}/render-fonts")
file(GLOB render_fonts LIST_DIRECTORIES false "${litehtml_tests_dir}/fonts/*.ttf" "${litehtml_tests_dir}/fonts/*.TTF")
file(COPY ${render_fonts} DESTINATION "${render_fonts_dir}")
configure_file(upstream/fonts.conf.in "${CMAKE_CURRENT_BINARY_DIR}/render-fonts.conf" @ONLY)

find_package(ZLIB REQUIRED)
add_sc_test(upstream LABELS upstream TIMEOUT 900 LINK_LIBRARIES sc-litehtml ZLIB::ZLIB)
set_tests_properties(test-upstream PROPERTIES ENVIRONMENT
        "FONTCONFIG_FILE=${CMAKE_CURRENT_BINARY_DIR}/render-fonts.conf;SC_LITEHTML_TESTS=${litehtml_tests_dir}/render;SC_UPSTREAM_PAGES=${CMAKE_CURRENT_SOURCE_DIR}/upstream/pages.txt;SC_UPSTREAM_FAILED=${CMAKE_CURRENT_BINARY_DIR}/upstream-failed")
