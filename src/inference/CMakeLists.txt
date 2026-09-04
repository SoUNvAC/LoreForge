qt_add_library(
    LoreForgeInference STATIC
    inference_types.cpp
    output_validator.cpp
)

add_library(LoreForge::Inference ALIAS LoreForgeInference)

target_include_directories(
    LoreForgeInference
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeInference
    PUBLIC
        LoreForge::Core
        LoreForge::ProjectOptions
        Qt6::Core
)
