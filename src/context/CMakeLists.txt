qt_add_library(
    LoreForgeContext STATIC
    context_builder.cpp
    context_engine.cpp
    context_types.cpp
    relevant_memory_retriever.cpp
)

add_library(LoreForge::Context ALIAS LoreForgeContext)

target_include_directories(
    LoreForgeContext
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeContext
    PUBLIC
        LoreForge::Inference
        LoreForge::Llm
        LoreForge::Narrative
        LoreForge::Storage
        LoreForge::Text
        LoreForge::ProjectOptions
        Qt6::Core
)
