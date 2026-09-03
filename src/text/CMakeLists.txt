qt_add_library(
    LoreForgeText STATIC
    chapter_heading_detector.cpp
    paragraph_detector.cpp
    text_normalizer.cpp
    word_counter.cpp
)

add_library(LoreForge::Text ALIAS LoreForgeText)

target_include_directories(
    LoreForgeText
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeText
    PUBLIC
        LoreForge::ProjectOptions
        Qt6::Core
)
