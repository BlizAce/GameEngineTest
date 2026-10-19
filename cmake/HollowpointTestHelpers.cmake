# Test helper functions shared by every test suite.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# hp_add_module_test(<ModuleName> SOURCES <files...> [LIBS <targets...>] [TIMEOUT <secs>]
#                    [LEAK_SUPPRESSIONS <file...>])
#
# Creates an executable <ModuleName>Tests, links it against doctest and the requested libraries, and
# registers it with CTest. Each module gets its own executable so a single subsystem can be exercised
# in isolation:
#   ctest -R CoreTests --output-on-failure
#
# Test data is read from the source tree (HP_DATA_DIR) rather than copied into the build directory,
# so a rebuilt binary can never test stale reference files.
#
# LEAK_SUPPRESSIONS names LeakSanitizer suppression files for this target only. It exists because tests that
# create windows or touch Vulkan load system libraries (GLFW -> libvulkan -> vendor driver -> D-Bus, plus
# GTK/GLib via libdecor on Wayland) which retain allocations past teardown and expose no teardown hook. Rather
# than disable leak detection for those suites — which would hide real leaks in the very code being tested — a
# file naming exactly those libraries is passed to LSAN. Suites that do not pass this run fully unsuppressed, so
# engine-side leak checking stays strict everywhere else.
# ---------------------------------------------------------------------------
function(hp_add_module_test MODULE_NAME)
    set(options "")
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs SOURCES LIBS LEAK_SUPPRESSIONS)
    cmake_parse_arguments(HP_TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(targetName "${MODULE_NAME}Tests")

    add_executable(${targetName}
        ${HP_TEST_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/Support/TestMain.cpp")

    hp_set_engine_target(${targetName})

    target_include_directories(${targetName} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Support")

    target_link_libraries(${targetName} PRIVATE
        doctest::doctest
        HollowpointTestSupport
        ${HP_TEST_LIBS})

    target_compile_definitions(${targetName} PRIVATE
        HP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/${MODULE_NAME}"
        HP_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Data")

    add_test(NAME ${targetName} COMMAND ${targetName} --order-by=lexical)

    if(NOT HP_TEST_TIMEOUT)
        set(HP_TEST_TIMEOUT 300)
    endif()

    # Leak suppressions go to LSAN_OPTIONS only where the caller asked for them. In a non-sanitiser build the
    # variable is simply ignored by the runtime, so setting it unconditionally is harmless and keeps this one code
    # path for both configurations.
    set(_hp_test_env "")
    if(HP_TEST_LEAK_SUPPRESSIONS)
        list(JOIN HP_TEST_LEAK_SUPPRESSIONS ":" _hp_suppression_paths)
        set(_hp_test_env "LSAN_OPTIONS=suppressions=${_hp_suppression_paths}")
    endif()

    # NOTE: a second set_tests_properties call replaces the whole ENVIRONMENT property, so every property is set
    # here in one place rather than split across blocks.
    set_tests_properties(${targetName} PROPERTIES
        TIMEOUT ${HP_TEST_TIMEOUT}
        LABELS "unit;${MODULE_NAME}")

    if(_hp_test_env)
        set_tests_properties(${targetName} PROPERTIES
            ENVIRONMENT "${_hp_test_env}")
    endif()

    set_target_properties(${targetName} PROPERTIES FOLDER "Tests")
endfunction()
