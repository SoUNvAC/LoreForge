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
loreforge_add_unit_test(LoreForgeSemanticDiffTests semantic_diff_test.cpp LoreForge::Git)
loreforge_add_unit_test(LoreForgeIncrementalAnalysisTests incremental_analysis_test.cpp LoreForge::Incremental)
loreforge_add_unit_test(LoreForgeNarrativeIntegrityTests narrative_integrity_test.cpp LoreForge::Narrative)
loreforge_add_unit_test(LoreForgeModelRegressionTests model_regression_test.cpp LoreForge::Regression)
target_compile_definitions(LoreForgeModelRegressionTests PRIVATE LOREFORGE_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
loreforge_add_unit_test(LoreForgeDocumentModelTests document_model_test.cpp)
loreforge_add_unit_test(LoreForgeDocumentJsonCodecTests document_json_codec_test.cpp)
loreforge_add_unit_test(LoreForgeTextProcessingTests text_processing_test.cpp LoreForge::Text)
loreforge_add_unit_test(LoreForgePlainTextParserTests plain_text_parser_test.cpp LoreForge::Parser)
loreforge_add_unit_test(LoreForgeMarkdownSourceParserTests markdown_source_parser_test.cpp LoreForge::Parser)

qt_add_executable(LoreForgeInferenceContractTests inference_contract_test.cpp)
target_link_libraries(
    LoreForgeInferenceContractTests
    PRIVATE
        LoreForge::Inference
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeInferenceContractTests COMMAND LoreForgeInferenceContractTests)

qt_add_executable(LoreForgeNarrativeExtractionTests narrative_extraction_test.cpp)
target_compile_definitions(
    LoreForgeNarrativeExtractionTests
    PRIVATE LOREFORGE_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
target_link_libraries(
    LoreForgeNarrativeExtractionTests
    PRIVATE
        LoreForge::Narrative
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeNarrativeExtractionTests COMMAND LoreForgeNarrativeExtractionTests)

qt_add_executable(LoreForgeChapterAnalysisTests chapter_analysis_test.cpp)
target_compile_definitions(
    LoreForgeChapterAnalysisTests
    PRIVATE LOREFORGE_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
target_link_libraries(
    LoreForgeChapterAnalysisTests
    PRIVATE
        LoreForge::Narrative
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeChapterAnalysisTests COMMAND LoreForgeChapterAnalysisTests)

qt_add_executable(LoreForgeStoryMemoryTests story_memory_test.cpp)
target_link_libraries(
    LoreForgeStoryMemoryTests
    PRIVATE
        LoreForge::Narrative
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeStoryMemoryTests COMMAND LoreForgeStoryMemoryTests)

qt_add_executable(LoreForgeContextEngineTests context_engine_test.cpp)
target_link_libraries(
    LoreForgeContextEngineTests
    PRIVATE
        LoreForge::Context
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeContextEngineTests COMMAND LoreForgeContextEngineTests)

qt_add_executable(LoreForgeProofreadingEngineTests proofreading_engine_test.cpp)
target_link_libraries(
    LoreForgeProofreadingEngineTests
    PRIVATE
        LoreForge::Proofreading
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeProofreadingEngineTests COMMAND LoreForgeProofreadingEngineTests)

qt_add_executable(LoreForgeLlmTransportTests llm_transport_test.cpp)
target_link_libraries(
    LoreForgeLlmTransportTests
    PRIVATE
        LoreForge::Llm
        LoreForge::ProjectOptions
        Qt6::Network
        Qt6::Test
)
add_test(NAME LoreForgeLlmTransportTests COMMAND LoreForgeLlmTransportTests)

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

qt_add_executable(LoreForgeMobiParserTests mobi_parser_test.cpp)
target_compile_definitions(
    LoreForgeMobiParserTests
    PRIVATE LOREFORGE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(
    LoreForgeMobiParserTests
    PRIVATE
        LoreForge::Document
        LoreForge::Parser
        LoreForge::ProjectOptions
        Qt6::Test
)
add_test(NAME LoreForgeMobiParserTests COMMAND LoreForgeMobiParserTests)

qt_add_executable(
    LoreForgeMainWindowTests
    main_window_test.cpp
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
        LoreForge::DesktopUi
        LoreForge::Storage
        LoreForge::ProjectOptions
        Qt6::Test
        Qt6::Widgets
)

add_test(NAME LoreForgeMainWindowTests COMMAND LoreForgeMainWindowTests)
set_tests_properties(LoreForgeMainWindowTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(LoreForgeLlmWorkbenchTests llm_workbench_test.cpp)
qt_add_executable(LoreForgeChapterPreviewTests chapter_preview_test.cpp)
qt_add_executable(LoreForgeChapterAnalysisPipelineTests chapter_analysis_pipeline_test.cpp)
target_include_directories(LoreForgeChapterAnalysisPipelineTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(LoreForgeChapterAnalysisPipelineTests PRIVATE LoreForge::DesktopUi Qt6::Test Qt6::Network)
add_test(NAME LoreForgeChapterAnalysisPipelineTests COMMAND LoreForgeChapterAnalysisPipelineTests)
set_tests_properties(LoreForgeChapterAnalysisPipelineTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_include_directories(LoreForgeChapterPreviewTests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(LoreForgeChapterPreviewTests PRIVATE LoreForge::DesktopUi Qt6::Test)
add_test(NAME LoreForgeChapterPreviewTests COMMAND LoreForgeChapterPreviewTests)
target_link_libraries(LoreForgeLlmWorkbenchTests PRIVATE LoreForge::DesktopUi Qt6::Test Qt6::Network)
add_test(NAME LoreForgeLlmWorkbenchTests COMMAND LoreForgeLlmWorkbenchTests)
set_tests_properties(LoreForgeLlmWorkbenchTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
