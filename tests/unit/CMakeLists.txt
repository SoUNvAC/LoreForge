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
    )
    add_test(NAME ${target} COMMAND ${target})
endfunction()

loreforge_add_unit_test(LoreForgeCoreTypesTests core_types_test.cpp)
loreforge_add_unit_test(LoreForgeDocumentModelTests document_model_test.cpp)
loreforge_add_unit_test(LoreForgeDocumentJsonCodecTests document_json_codec_test.cpp)
