cmake_minimum_required(VERSION 3.25)

project(Oscilline
    VERSION 0.1.0
    DESCRIPTION "Oscilline behavioral reimplementation engine"
    LANGUAGES CXX
)
if(WIN32)
    # The icon and version resources in packaging/windows.
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(OSCILLINE_BUILD_TESTS "Build unit tests" ON)
option(OSCILLINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OSCILLINE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(OSCILLINE_FETCH_SDL3 "Download SDL3 when it is not installed. Off requires an installed SDL3" ON)
option(OSCILLINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors. Release packages turn this off" ON)
option(OSCILLINE_PACKAGING "Build a Windows GUI executable and a macOS app bundle; see cmake/Packaging.cmake" OFF)
option(OSCILLINE_ENABLE_CLANG_TIDY "Run clang-tidy (.clang-tidy) on project sources while compiling" OFF)
if(OSCILLINE_ENABLE_CLANG_TIDY)
    find_program(OSCILLINE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

if(OSCILLINE_ENABLE_SANITIZERS AND OSCILLINE_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined")
endif()

if(OSCILLINE_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "OSCILLINE_ENABLE_SANITIZERS is supported with Clang and GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(OSCILLINE_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "OSCILLINE_ENABLE_TSAN is supported with Clang and GCC")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

include(FetchContent)

if(OSCILLINE_FETCH_SDL3)
    find_package(SDL3 QUIET)
else()
    # Offline builds (Flatpak, distributions) provide SDL3 themselves.
    find_package(SDL3 REQUIRED CONFIG)
endif()
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found on the system; fetching release-3.4.18")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE
        # CI deletes the source's .git before caching it. Without this, a
        # CMake re-run during the build would try to update the checkout.
        UPDATE_DISCONNECTED TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

configure_file(
    include/oscilline/version_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/oscilline/version_config.hpp
    @ONLY
)

add_library(oscilline_core STATIC
    src/version.cpp
    src/disc/sector.cpp
    src/disc/cue.cpp
    src/disc/image.cpp
    src/disc/iso9660.cpp
    src/disc/identity.cpp
    src/pak.cpp
    src/tim.cpp
    src/tmd.cpp
    src/anm.cpp
    src/adpcm.cpp
    src/vab.cpp
    src/fsl.cpp
    src/inspect.cpp
    src/course/obstacle.cpp
    src/course/mapping.cpp
    src/course/beats.cpp
    src/course/attack.cpp
    src/course/load.cpp
    src/course/play.cpp
    src/course/score.cpp
    src/course/camera.cpp
    src/course/camera_schedule.cpp
    src/course/jitter.cpp
    src/course/shapes.cpp
    src/course/difficulty.cpp
    src/audio/cdda.cpp
    src/audio/sfx.cpp
    src/audio/decode_music.cpp
    src/course/music.cpp
    src/render/clock.cpp
    src/render/stroke.cpp
    src/render/project.cpp
    src/disc/archive.cpp
    src/asset/map.cpp
    src/asset/registry.cpp
    src/asset/fallback.cpp
    src/asset/meter.cpp
    src/anc.cpp
    src/asset/character.cpp
    src/asset/menu.cpp
    src/course/figure.cpp
    src/text/vector_font.cpp
    src/settings.cpp
    src/controls.cpp
)
target_include_directories(oscilline_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(oscilline_core PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(oscilline_core PUBLIC Threads::Threads)

function(oscilline_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /external:W0 /external:anglebrackets)
        if(OSCILLINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(OSCILLINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        # GCC 12 reports a false -Wrestrict on "text" + std::string at -O2 and
        # above (GCC bug 105651).
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            target_compile_options(${target} PRIVATE -Wno-restrict)
        endif()
    endif()
    # Applied per target, so fetched dependencies such as SDL are not checked.
    if(OSCILLINE_ENABLE_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${OSCILLINE_CLANG_TIDY_EXE}")
    endif()
endfunction()

oscilline_warnings(oscilline_core)
if(MSVC)
    set_source_files_properties(src/audio/decode_music.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/audio/decode_music.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
target_include_directories(oscilline_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs)

add_library(oscilline_present STATIC
    src/present/host.cpp
    src/present/cdda_out.cpp
    src/present/sfx_out.cpp
)
target_link_libraries(oscilline_present PUBLIC oscilline_core SDL3::SDL3)
oscilline_warnings(oscilline_present)

add_executable(oscilline
    src/app/main.cpp
    src/app/session.cpp
    src/app/course_draw.cpp
    src/app/score_tracker.cpp
    src/app/title.cpp
    src/app/text_out.cpp
    src/app/sfx_stage.cpp
)
target_link_libraries(oscilline PRIVATE oscilline_present)
oscilline_warnings(oscilline)

include(cmake/Packaging.cmake)

add_executable(oscilline-viewer src/viewer/main.cpp)
target_link_libraries(oscilline-viewer PRIVATE oscilline_present)
oscilline_warnings(oscilline-viewer)

add_executable(oscilline-extract src/extract/main.cpp)
target_link_libraries(oscilline-extract PRIVATE oscilline_core)
oscilline_warnings(oscilline-extract)

add_executable(oscilline-chart src/chart/main.cpp)
target_link_libraries(oscilline-chart PRIVATE oscilline_core)
oscilline_warnings(oscilline-chart)

add_executable(oscilline-inspect src/inspect/main.cpp)
target_link_libraries(oscilline-inspect PRIVATE oscilline_core)
oscilline_warnings(oscilline-inspect)

if(OSCILLINE_BUILD_TESTS)
    find_package(doctest QUIET)
    if(NOT doctest_FOUND)
        message(STATUS "doctest not found on the system; fetching v2.5.3")
        # Tests only; keep doctest out of the installed package.
        set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            doctest
            GIT_REPOSITORY https://github.com/doctest/doctest.git
            GIT_TAG v2.5.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(doctest)
    endif()

    enable_testing()
    add_executable(oscilline_tests
        tests/test_main.cpp
        tests/version_test.cpp
        tests/disc_test.cpp
        tests/asset_test.cpp
        tests/render_test.cpp
        tests/course_test.cpp
        tests/score_test.cpp
        tests/cdda_test.cpp
        tests/music_test.cpp
        tests/asset_registry_test.cpp
        tests/font_test.cpp
        tests/meter_test.cpp
        tests/sfx_test.cpp
        tests/character_test.cpp
        tests/anc_test.cpp
        tests/camera_schedule_test.cpp
        tests/course_draw_test.cpp
        tests/menu_test.cpp
        tests/settings_test.cpp
        tests/controls_test.cpp
        src/app/course_draw.cpp
    src/app/score_tracker.cpp
        src/app/sfx_stage.cpp
        src/app/text_out.cpp
    )
    target_include_directories(oscilline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
    add_dependencies(oscilline_tests oscilline-extract oscilline-inspect)
    target_link_libraries(oscilline_tests PRIVATE oscilline_present doctest::doctest)
    oscilline_warnings(oscilline_tests)
    add_test(NAME oscilline_tests COMMAND oscilline_tests)

    add_test(NAME oscilline_smoke COMMAND oscilline --frames 2)
    set_tests_properties(oscilline_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
    )

    add_test(NAME oscilline_play_needs_disc COMMAND oscilline --play)
    set_tests_properties(oscilline_play_needs_disc PROPERTIES
        WILL_FAIL TRUE
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
    )

    add_test(NAME oscilline_frames_rejects_text COMMAND oscilline --frames abc)
    set_tests_properties(oscilline_frames_rejects_text PROPERTIES
        WILL_FAIL TRUE
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
    )

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/click.wav
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_click_wav.py ${CMAKE_CURRENT_BINARY_DIR}/click.wav
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_click_wav.py
        VERBATIM
    )
    add_custom_target(oscilline_click_wav ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/click.wav)
    add_dependencies(oscilline oscilline_click_wav)

    add_test(NAME oscilline_course_without_play COMMAND oscilline --course 1 --frames 1)
    set_tests_properties(oscilline_course_without_play PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "without --play does not start that course"
    )

    add_test(NAME oscilline_music_smoke
        COMMAND oscilline --frames 2 --play --music ${CMAKE_CURRENT_BINARY_DIR}/click.wav --difficulty bronze
    )
    set_tests_properties(oscilline_music_smoke PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    )

    add_test(NAME oscilline_asset_report COMMAND oscilline --asset-report --frames 1)
    set_tests_properties(oscilline_asset_report PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "font +placeholder"
    )

    add_test(NAME oscilline_no_disc_assets COMMAND oscilline --no-disc-assets --asset-report --frames 1)
    set_tests_properties(oscilline_no_disc_assets PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "font +placeholder"
    )

    add_test(NAME oscilline_sfx_test COMMAND oscilline --sfx-test --frames 2)
    set_tests_properties(oscilline_sfx_test PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    )

    add_test(NAME oscilline_help COMMAND oscilline --help)
    set_tests_properties(oscilline_help PROPERTIES
        PASS_REGULAR_EXPRESSION "--ribbon-guides"
    )
    add_test(NAME oscilline_help_builtin_camera COMMAND oscilline --help)
    set_tests_properties(oscilline_help_builtin_camera PROPERTIES
        PASS_REGULAR_EXPRESSION "--builtin-camera"
    )

    add_test(NAME viewer_help COMMAND oscilline-viewer --help)
    add_test(NAME viewer_needs_disc COMMAND oscilline-viewer --frames 1)
    set_tests_properties(viewer_needs_disc PROPERTIES WILL_FAIL TRUE)
    add_test(NAME viewer_sfx_list COMMAND oscilline-viewer --sfx --frames 1)
    set_tests_properties(viewer_sfx_list PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "sfx 0 menu-move"
    )
    add_test(NAME viewer_sfx_unidentified COMMAND oscilline-viewer --sfx --frames 1)
    set_tests_properties(viewer_sfx_unidentified PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
        PASS_REGULAR_EXPRESSION "sfx unidentified count 0"
    )
    add_test(NAME viewer_help_sfx_tab COMMAND oscilline-viewer --help)
    set_tests_properties(viewer_help_sfx_tab PROPERTIES
        PASS_REGULAR_EXPRESSION "Tab opens the SFX browser"
    )

    if(OSCILLINE_ENABLE_TSAN)
        set_tests_properties(
            oscilline_tests
            oscilline_smoke
            oscilline_play_needs_disc
            oscilline_frames_rejects_text
            oscilline_course_without_play
            oscilline_music_smoke
            viewer_help
            viewer_needs_disc
            viewer_sfx_list
            viewer_sfx_unidentified
            viewer_help_sfx_tab
            PROPERTIES
                ENVIRONMENT_MODIFICATION "TSAN_OPTIONS=set:suppressions=${CMAKE_SOURCE_DIR}/cmake/tsan.supp"
        )
    endif()
endif()

