qt_add_library(LoreForgeRegression STATIC model_regression.cpp golden_corpus.cpp)
add_library(LoreForge::Regression ALIAS LoreForgeRegression)
target_include_directories(LoreForgeRegression PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(LoreForgeRegression PUBLIC LoreForge::Core LoreForge::Narrative LoreForge::Llm LoreForge::ProjectOptions Qt6::Core)
