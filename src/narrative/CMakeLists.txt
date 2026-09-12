qt_add_library(
    LoreForgeNarrative STATIC
    dialogue_extractor.cpp
    extraction_evaluator.cpp
    narrative_types.cpp
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
