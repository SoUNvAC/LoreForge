qt_add_executable(
    LoreForgeDocumentFixtureTests
    document_fixture_test.cpp
)

target_include_directories(
    LoreForgeDocumentFixtureTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/support
)

target_compile_definitions(
    LoreForgeDocumentFixtureTests
    PRIVATE
        LOREFORGE_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)

target_link_libraries(
    LoreForgeDocumentFixtureTests
    PRIVATE
        LoreForge::Document
        LoreForge::ProjectOptions
        Qt6::Test
)

add_test(NAME LoreForgeDocumentFixtureTests COMMAND LoreForgeDocumentFixtureTests)

qt_add_executable(
    LoreForgePlainTextImportFixtureTests
    plain_text_import_fixture_test.cpp
)

target_include_directories(
    LoreForgePlainTextImportFixtureTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/support
)

target_compile_definitions(
    LoreForgePlainTextImportFixtureTests
    PRIVATE
        LOREFORGE_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)

target_link_libraries(
    LoreForgePlainTextImportFixtureTests
    PRIVATE
        LoreForge::Document
        LoreForge::Parser
        LoreForge::ProjectOptions
        Qt6::Test
)

add_test(NAME LoreForgePlainTextImportFixtureTests COMMAND LoreForgePlainTextImportFixtureTests)
