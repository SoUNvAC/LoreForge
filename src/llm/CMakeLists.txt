qt_add_library(
    LoreForgeLlm STATIC
    ${PROJECT_SOURCE_DIR}/include/loreforge/llm/illm_client.h
    ${PROJECT_SOURCE_DIR}/include/loreforge/llm/llm_types.h
    ${PROJECT_SOURCE_DIR}/include/loreforge/llm/qwen_client.h
    qwen_client.cpp
)

add_library(LoreForge::Llm ALIAS LoreForgeLlm)

target_include_directories(
    LoreForgeLlm
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeLlm
    PUBLIC
        LoreForge::ProjectOptions
        Qt6::Core
        Qt6::Network
)
