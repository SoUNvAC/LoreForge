option(LOREFORGE_ENABLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LOREFORGE_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

add_library(loreforge_project_options INTERFACE)
add_library(LoreForge::ProjectOptions ALIAS loreforge_project_options)

if(MSVC)
    target_compile_options(
        loreforge_project_options
        INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            $<$<BOOL:${LOREFORGE_ENABLE_WARNINGS_AS_ERRORS}>:/WX>
    )
else()
    target_compile_options(
        loreforge_project_options
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${LOREFORGE_ENABLE_WARNINGS_AS_ERRORS}>:-Werror>
    )
endif()

if(LOREFORGE_ENABLE_CLANG_TIDY)
    find_program(LOREFORGE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${LOREFORGE_CLANG_TIDY_EXE}" CACHE STRING "clang-tidy command" FORCE)
endif()
