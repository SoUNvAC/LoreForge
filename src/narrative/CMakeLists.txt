qt_add_library(
    LoreForgeNarrative STATIC
    chapter_analysis.cpp
    chapter_analyzer.cpp
    dialogue_extractor.cpp
    extraction_evaluator.cpp
    narrative_types.cpp
    story_memory.cpp
    story_memory_json.cpp
)

add_library(LoreForge::Narrative ALIAS LoreForgeNarrative)

target_include_directories(
    LoreForgeNarrative
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeNarrative
    PUBLIC
        LoreForge::Core
        LoreForge::Inference
        LoreForge::ProjectOptions
        Qt6::Core
)
