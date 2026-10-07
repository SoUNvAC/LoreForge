qt_add_library(
    LoreForgeParser STATIC
    epub_parser.cpp
    mobi_parser.cpp
    pdf_parser.cpp
    plain_text_parser.cpp
    markdown_source_parser.cpp
)

add_library(LoreForge::Parser ALIAS LoreForgeParser)

target_include_directories(
    LoreForgeParser
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeParser
    PUBLIC
        LoreForge::Document
        LoreForge::Text
        LoreForge::ProjectOptions
        Qt6::Core
        Qt6::Pdf
    PRIVATE
        Qt6::CorePrivate
)

qt_add_executable(LoreForgeMarkdownInspect markdown_inspect.cpp)
target_link_libraries(LoreForgeMarkdownInspect PRIVATE LoreForge::Parser LoreForge::ProjectOptions)
