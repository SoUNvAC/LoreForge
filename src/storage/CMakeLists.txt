qt_add_library(
    LoreForgeStorage STATIC
    book_repository.cpp
    chapter_repository.cpp
    project_database.cpp
    project_repository.cpp
)

add_library(LoreForge::Storage ALIAS LoreForgeStorage)

target_include_directories(
    LoreForgeStorage
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(
    LoreForgeStorage
    PUBLIC
        LoreForge::Document
        LoreForge::ProjectOptions
        Qt6::Core
        Qt6::Sql
)

qt_add_resources(
    LoreForgeStorage storage_migrations
    PREFIX "/loreforge/migrations"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/migrations"
    FILES
        migrations/001_initial.sql
)
