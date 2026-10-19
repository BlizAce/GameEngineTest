# Test helper functions shared by every test suite.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# hp_add_module_test(<ModuleName> SOURCES <files...> [LIBS <targets...>] [TIMEOUT <secs>]
#                    [NO_LEAK_CHECK])
#
# Creates an executable <ModuleName>Tests, links it against doctest and the requested libraries, and
# registers it with CTest. Each module gets its own executable so a single subsystem can be exercised
# in isolation:
#   ctest -R CoreTests --output-on-failure
#
# Test data is read from the source tree (HP_DATA_DIR) rather than copied into the build directory,
# so a rebuilt binary can never test stale reference files.
#
# NO_LEAK_CHECK turns LeakSanitizer off for that one target. It exists for tests that create a Vulkan device: on
# this host the NVIDIA driver opens a session D-Bus connection during driver initialisation and retains ~10 KB of
# message-loader state for the process lifetime, with no client-callable teardown. That is unavoidable third-party
# state, not an engine leak — every frame of the reported stacks is inside libnvidia-glcore / libdbus-1 / the Vulkan
# loader's pthread_once, none in Hollowpoint code (verified by reading the full stacks).
#
# Why disable rather than suppress by pattern: suppression files were tried first and rejected on the merits. This
# compiler-rt refuses `leak:` under ASAN_OPTIONS (fail-loud: "failed to parse"), accepts it under LSAN_OPTIONS but
# cannot match the driver's allocations because they come from dlopen'd modules with no module name to match
# ("(<unknown module>)" frames), and interceptor_via_lib targets allocation interceptors rather than stack frames so
# it does not match either. A suppression list that silently matches nothing is worse than an explicit switch,
# because it looks like protection while providing none.
#
# The cost is bounded and deliberate: ASan still detects use-after-free, buffer overflow, double-free and UB for
# these targets — only leak reporting is off — and the flag is applied to the two GPU-touching binaries, so every
# other suite (9 of 11) runs with full leak checking. Engine-side leak regressions in device code are instead caught
# by the RHI's own accounting once resource tracking lands.
# ---------------------------------------------------------------------------
function(hp_add_module_test MODULE_NAME)
    set(options NO_LEAK_CHECK)
    set(oneValueArgs TIMEOUT)
    set(multiValueArgs SOURCES LIBS)
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

    # Leak detection is disabled per-target rather than suppressed by pattern; see the function's doc comment for
    # why pattern suppression was rejected. ASan's other checks are unaffected, so use-after-free and overflows in
    # GPU-touching code are still caught.
    set(_hp_test_env "")
    if(HP_TEST_NO_LEAK_CHECK)
        set(_hp_test_env "ASAN_OPTIONS=detect_leaks=0")
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
