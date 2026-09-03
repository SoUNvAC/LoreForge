add_subdirectory(unit)
add_subdirectory(integration)

add_test(NAME LoreForgeApplicationSmoke COMMAND LoreForge --smoke-test)
set_tests_properties(
    LoreForgeApplicationSmoke
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 10
)
