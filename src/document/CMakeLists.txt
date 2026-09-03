qt_add_library(
    LoreForgeDocument STATIC
    document.cpp
    document_hash.cpp
    document_json_codec.cpp
    document_validation.cpp
)

add_library(LoreForge::Document ALIAS LoreForgeDocument)

target_include_directories(
    LoreForgeDocument
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeDocument
    PUBLIC
        LoreForge::Core
        LoreForge::ProjectOptions
        Qt6::Core
)
