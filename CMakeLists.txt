cmake_minimum_required(VERSION 3.25)

project(
    LoreForge
    VERSION 0.1.0
    DESCRIPTION "A desktop workspace for versioned narrative source trees"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(cmake/ProjectOptions.cmake)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Sql Widgets)
qt_standard_project_setup(REQUIRES 6.5)

add_subdirectory(src)

if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
