qt_add_library(
    LoreForgeGit STATIC
    git_repository.cpp
)

add_library(LoreForge::Git ALIAS LoreForgeGit)

target_include_directories(
    LoreForgeGit
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeGit
    PUBLIC
        LoreForge::Core
        LoreForge::Proofreading
        LoreForge::ProjectOptions
        Qt6::Core
)
