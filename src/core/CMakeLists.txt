qt_add_library(
    LoreForgeCore STATIC
    application_config.cpp
    build_info.cpp
    content_hash.cpp
    logging.cpp
    source_span.cpp
)

add_library(LoreForge::Core ALIAS LoreForgeCore)

target_include_directories(
    LoreForgeCore
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeCore
    PUBLIC
        Qt6::Core
        LoreForge::ProjectOptions
)

target_compile_definitions(LoreForgeCore PRIVATE LOREFORGE_VERSION="${PROJECT_VERSION}")
