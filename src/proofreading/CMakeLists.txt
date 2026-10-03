qt_add_library(
    LoreForgeProofreading STATIC
    deterministic_proofreader.cpp
    proofreading_support.cpp
    proofreading_types.cpp
    protected_term_registry.cpp
    repair_queue.cpp
    semantic_proofreader.cpp
)

add_library(LoreForge::Proofreading ALIAS LoreForgeProofreading)

target_include_directories(
    LoreForgeProofreading
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    LoreForgeProofreading
    PUBLIC
        LoreForge::Core
        LoreForge::Inference
        LoreForge::ProjectOptions
        Qt6::Core
)
