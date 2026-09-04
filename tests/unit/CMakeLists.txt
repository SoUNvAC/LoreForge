qt_add_executable(
    LoreForgeApplicationConfigTests
    application_config_test.cpp
)

target_link_libraries(
    LoreForgeApplicationConfigTests
    PRIVATE
        LoreForge::Core
        LoreForge::ProjectOptions
        Qt6::Test
)

add_test(NAME LoreForgeApplicationConfigTests COMMAND LoreForgeApplicationConfigTests)

qt_add_executable(
    LoreForgeBuildInfoTests
    build_info_test.cpp
)

target_link_libraries(
    LoreForgeBuildInfoTests
    PRIVATE
        LoreForge::Core
        LoreForge::ProjectOptions
        Qt6::Test
)

add_test(NAME LoreForgeBuildInfoTests COMMAND LoreForgeBuildInfoTests)

function(loreforge_add_unit_test target source)
    qt_add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    target_link_libraries(
        ${target}
        PRIVATE
            LoreForge::Document
            LoreForge::ProjectOptions
            Qt6::Test
            ${ARGN}
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

loreforge_add_unit_test(LoreForgeCoreTypesTests core_types_test.cpp)
loreforge_add_unit_test(LoreForgeDocumentModelTests document_model_test.cpp)
loreforge_add_unit_test(LoreForgeDocumentJsonCodecTests document_json_codec_test.cpp)
loreforge_add_unit_test(LoreForgeTextProcessingTests text_processing_test.cpp LoreForge::Text)
loreforge_add_unit_test(LoreForgePlainTextParserTests plain_text_parser_test.cpp LoreForge::Parser)

qt_add_executable(LoreForgeEpubParserTests epub_parser_test.cpp)
target_include_directories(LoreForgeEpubParserTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_definitions(
    LoreForgeEpubParserTests
    PRIVATE LOREFORGE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
    LoreForgeEpubParserTests
    PRIVATE
        LoreForge::Document
        LoreForge::Parser
        LoreForge::ProjectOptions
        Qt6::CorePrivate
        Qt6::Test
)
add_test(NAME LoreForgeEpubParserTests COMMAND LoreForgeEpubParserTests)

qt_add_executable(LoreForgePdfParserTests pdf_parser_test.cpp)
target_compile_definitions(
    LoreForgePdfParserTests
    PRIVATE LOREFORGE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
    LoreForgePdfParserTests
    PRIVATE
        LoreForge::Document
        LoreForge::Parser
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgePdfParserTests COMMAND LoreForgePdfParserTests)

qt_add_executable(
    LoreForgeMainWindowTests
    main_window_test.cpp
    ${PROJECT_SOURCE_DIR}/src/app/main_window.cpp
    ${PROJECT_SOURCE_DIR}/src/app/main_window.h
)

target_include_directories(
    LoreForgeMainWindowTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/app
        ${PROJECT_SOURCE_DIR}/tests/support
)

target_link_libraries(
    LoreForgeMainWindowTests
    PRIVATE
        LoreForge::Parser
        LoreForge::Text
        LoreForge::ProjectOptions
        Qt6::Test
        Qt6::Widgets
)

add_test(NAME LoreForgeMainWindowTests COMMAND LoreForgeMainWindowTests)
set_tests_properties(LoreForgeMainWindowTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
