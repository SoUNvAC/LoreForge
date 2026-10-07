qt_add_library(LoreForgeIncremental STATIC dependency_graph.cpp dependency_checkpoint.cpp chapter_pipeline.cpp)
add_library(LoreForge::Incremental ALIAS LoreForgeIncremental)
target_include_directories(LoreForgeIncremental PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(LoreForgeIncremental PUBLIC LoreForge::Core LoreForge::Git LoreForge::ProjectOptions Qt6::Core)
