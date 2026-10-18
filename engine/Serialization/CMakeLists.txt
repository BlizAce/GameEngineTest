# Hollowpoint Serialization: JSON document model, parser and writer, plus the reflection-driven value bridge.
#
# Depends on Core only (not Math or Reflection) so that the document layer stays a leaf everyone can use. The
# reflection-driven encode/decode lives in a separate target below to keep that property.

add_library(HollowpointSerialization STATIC)
hp_set_engine_target(HollowpointSerialization)

target_include_directories(HollowpointSerialization PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")

target_sources(HollowpointSerialization PRIVATE
    Include/Hollowpoint/Serialization/JsonParser.h
    Include/Hollowpoint/Serialization/JsonValue.h
    Include/Hollowpoint/Serialization/JsonWriter.h
    Src/JsonParser.cpp
    Src/JsonValue.cpp
    Src/JsonWriter.cpp)

# JSON is a pure data layer: Core for types, logging and assertions, nothing else. Deliberately not linking nlohmann
# — the engine needs exact control over number formatting (shortest round-trip reals), key order (insertion, so scene
# files diff per field) and error reporting (byte offset plus line/column with context), none of which a general JSON
# library exposes. See notes/EXPERIMENTS.md.
target_link_libraries(HollowpointSerialization
    PUBLIC Hollowpoint::Core Hollowpoint::CompilerSetup)

add_library(Hollowpoint::Serialization ALIAS HollowpointSerialization)
