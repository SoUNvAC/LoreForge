qt_add_library(
    LoreForgeDesktopUi STATIC
    document_metrics.cpp
    document_metrics.h
    main_window.cpp
    main_window.h
    project_workspace.cpp
    project_workspace.h
)

add_library(LoreForge::DesktopUi ALIAS LoreForgeDesktopUi)

target_include_directories(
    LoreForgeDesktopUi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    LoreForgeDesktopUi
    PUBLIC
        LoreForge::Storage
        LoreForge::Text
        LoreForge::ProjectOptions
        Qt6::Widgets
)

qt_add_executable(
    LoreForge
    WIN32
    main.cpp
)

target_link_libraries(
    LoreForge
    PRIVATE
        LoreForge::Core
        LoreForge::DesktopUi
        LoreForge::ProjectOptions
        Qt6::Widgets
)

set_target_properties(
    LoreForge
    PROPERTIES
        OUTPUT_NAME "LoreForge"
)

install(TARGETS LoreForge RUNTIME DESTINATION bin)
