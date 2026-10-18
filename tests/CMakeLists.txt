# Unit tests.
#
# Layout: one executable per engine module (CoreTests, MathTests, ...) so a failing module can be run
# on its own, plus a shared support library. Every executable registers with CTest.

add_subdirectory(Support)

include(HollowpointTestHelpers)

if(TARGET HollowpointCore)
    hp_add_module_test(Core
        SOURCES
            Core/TestAssert.cpp
            Core/TestGuid.cpp
            Core/TestHash.cpp
            Core/TestLog.cpp
            Core/TestMemory.cpp
            Core/TestRefCounted.cpp
            Core/TestTime.cpp
        LIBS Hollowpoint::Core)
endif()
