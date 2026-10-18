# Test support library: shared main(), assertion capture, helpers.
add_library(HollowpointTestSupport STATIC
    TestMain.cpp
    TestSupport.cpp)

hp_set_engine_target(HollowpointTestSupport)

target_include_directories(HollowpointTestSupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(HollowpointTestSupport PUBLIC
    doctest::doctest
    Hollowpoint::Core)
