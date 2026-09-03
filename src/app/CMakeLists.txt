qt_add_executable(
    LoreForge
    WIN32
    main.cpp
    main_window.cpp
    main_window.h
)

target_link_libraries(
    LoreForge
    PRIVATE
        LoreForge::Core
        LoreForge::Parser
        LoreForge::Text
        LoreForge::ProjectOptions
        Qt6::Widgets
)

set_target_properties(
    LoreForge
    PROPERTIES
        OUTPUT_NAME "LoreForge"
)

install(TARGETS LoreForge RUNTIME DESTINATION bin)
