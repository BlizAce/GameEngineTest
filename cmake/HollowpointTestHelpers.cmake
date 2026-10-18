# Test helper functions shared by every test suite.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# hp_add_module_test(<ModuleName> SOURCES <files...> [LIBS <targets...>] [TIMEOUT <secs>])
#
# Creates an executable <ModuleName>Tests, links it against doctest and the requested libraries, and
# registers it with CTest. Each module gets its own executable so a single subsystem can be exercised
# in isolation:
#   ctest -R CoreTests --output-on-failure
#
# Test data is read from the source tree (HP_DATA_DIR) rather than copied into the build directory,
# so a rebuilt binary can never test stale reference files.
# ---------------------------------------------------------------------------
function(hp_add_module_test MODULE_NAME)
    set(options "")
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

    set_tests_properties(${targetName} PROPERTIES
        TIMEOUT ${HP_TEST_TIMEOUT}
        LABELS "unit;${MODULE_NAME}")

    set_target_properties(${targetName} PROPERTIES FOLDER "Tests")
endfunction()
